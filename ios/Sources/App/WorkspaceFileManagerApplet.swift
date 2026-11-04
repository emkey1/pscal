// Copyright 2026 Mike Miller
// SPDX-License-Identifier: Apache-2.0
//
// Ported by its author from iSH-AOK's Workspace (app/WorkspaceFileManager.m at 0f75fee1c) and
// licensed to PSCAL under the Apache License 2.0.

import UIKit

/// Which applet shows a path, going by its name. A known-binary type has no route rather than
/// a guess, and neither has anything that isn't a file or a folder.
@MainActor
enum WorkspaceFileOpenRouter {
    fileprivate enum Category {
        case markdown, image, video, browser, audio, text, binary, unknown
    }

    private static let markdownExtensions: Set<String> = ["md", "markdown"]
    // Not svg, which the image viewer can't rasterize: that goes to the Browser.
    private static let imageExtensions: Set<String> = [
        "png", "jpg", "jpeg", "gif", "bmp", "tiff", "tif", "heic", "heif", "webp", "ico",
    ]
    // Includes containers AVFoundation may not play (mkv, webm), so those get the player's
    // real error rather than a flat "no viewer" from here. m3u and m3u8 are playlists, which
    // the player reads and lists.
    private static let videoExtensions: Set<String> = ["mp4", "mov", "m4v", "avi", "mkv", "webm", "m3u", "m3u8"]
    // Web pages, SVG and PDF: WebKit renders all three.
    private static let browserExtensions: Set<String> = ["html", "htm", "xhtml", "svg", "pdf"]
    private static let audioExtensions: Set<String> = [
        "mp3", "m4a", "mp4", "aac", "adts", "alac", "wav", "wave", "aif", "aiff", "aifc", "caf", "flac",
        "ogg", "oga", "opus",
    ]
    private static let textExtensions: Set<String> = [
        "txt", "log", "conf", "cfg", "config", "json", "yaml", "yml",
        "xml", "csv", "tsv", "sh", "bash", "zsh", "c", "h", "cpp", "cc", "hpp", "m", "mm",
        "py", "rb", "js", "ts", "jsx", "tsx", "go", "rs", "java", "swift", "ini", "toml",
        "gitignore", "env", "plist", "properties", "gradle", "make", "mk", "cmake", "rst",
        "tex", "diff", "patch",
    ]
    private static let binaryExtensions: Set<String> = [
        "zip", "tar", "gz", "bz2", "xz", "7z", "rar",
        "doc", "docx", "xls", "xlsx", "ppt", "pptx",
        "so", "dylib", "dll", "exe", "o", "a", "bin",
        "db", "sqlite", "sqlite3",
        "ttf", "otf", "woff", "woff2",
    ]

    fileprivate static func category(ofPath path: String) -> Category {
        let ext = (path as NSString).pathExtension.lowercased()
        if markdownExtensions.contains(ext) { return .markdown }
        if imageExtensions.contains(ext) { return .image }
        if videoExtensions.contains(ext) { return .video }
        if browserExtensions.contains(ext) { return .browser }
        if audioExtensions.contains(ext) { return .audio }
        if textExtensions.contains(ext) { return .text }
        if binaryExtensions.contains(ext) { return .binary }
        return .unknown
    }

    /// A `WorkspaceToolRegistry` identifier, which may not be available yet.
    static func tool(forPath path: String, kind: WorkspaceFileKind) -> String? {
        switch kind {
        case .directory:
            return WorkspaceToolRegistry.fileManager
        case .regular:
            break
        default:
            return nil
        }
        switch category(ofPath: path) {
        case .markdown: return WorkspaceToolRegistry.markdown
        case .image: return WorkspaceToolRegistry.imageViewer
        case .video: return WorkspaceToolRegistry.videoPlayer
        case .browser: return WorkspaceToolRegistry.browser
        case .audio: return WorkspaceToolRegistry.music
        case .text, .unknown: return WorkspaceToolRegistry.motePad
        case .binary: return nil
        }
    }

    /// The name says nothing, so `tool(forPath:kind:)` assumes text. Worth looking inside
    /// before believing that.
    static func dependsOnContent(path: String) -> Bool {
        category(ofPath: path) == .unknown
    }
}

/// A list entry and the link target to show for it, which is worked out off the main thread:
/// turning a real path into the shell's takes the lock path expansion holds.
private struct WorkspaceFileRow {
    let item: WorkspaceFileItem
    let linkTarget: String?
}

private struct WorkspaceFilePlace: Equatable {
    let title: String
    let path: String
    let symbol: String
}

private enum WorkspaceFileSortMode: Int, CaseIterable {
    case name = 0, size, date, kind

    var title: String {
        switch self {
        case .name: return "Name"
        case .size: return "Size"
        case .date: return "Date Modified"
        case .kind: return "Kind"
        }
    }
}

private extension WorkspaceFileKind {
    var sortRank: Int {
        switch self {
        case .regular: return 0
        case .directory: return 1
        case .symlink: return 2
        case .fifo: return 3
        case .socket: return 4
        case .charDevice: return 5
        case .blockDevice: return 6
        case .other: return 7
        }
    }
}

/// The File Manager's work that the file service has no call for, all of it off the main thread.
private enum WorkspaceFileManagerWork {
    static func run<Value>(_ work: @escaping @Sendable () -> Value,
                           completion: @escaping @MainActor (Value) -> Void) {
        DispatchQueue.global(qos: .userInitiated).async {
            nonisolated(unsafe) let value = work()
            DispatchQueue.main.async {
                MainActor.assumeIsolated {
                    completion(value)
                }
            }
        }
    }

    /// The fixed places first, home at the front, then what's been mounted.
    static func places() -> [WorkspaceFilePlace] {
        let documents = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask).first?.path
            ?? (NSHomeDirectory() as NSString).appendingPathComponent("Documents")
        // Until the runtime has started, HOME is still the system's, the app's container.
        var hostHome = (documents as NSString).appendingPathComponent("home")
        if let value = getenv("HOME") {
            let home = String(cString: value)
            if !home.isEmpty && home != NSHomeDirectory() {
                hostHome = home
            }
        }
        let home = WorkspaceFileService.shownPath(hostHome)
        let temporary = WorkspaceFileService.shownPath((documents as NSString).appendingPathComponent("tmp"))
        var places = [
            WorkspaceFilePlace(title: "Home", path: home, symbol: "house"),
            WorkspaceFilePlace(title: "Docs", path: WorkspaceFileService.join(home, "Docs"), symbol: "book"),
            WorkspaceFilePlace(title: "/tmp", path: temporary, symbol: "clock"),
            WorkspaceFilePlace(title: "Root (/)", path: "/", symbol: "internaldrive"),
        ]
        for target in WorkspaceFileService.mountTargets() {
            places.append(WorkspaceFilePlace(title: target, path: target, symbol: "externaldrive"))
        }
        return places
    }

    static func shownLinkTarget(_ item: WorkspaceFileItem) -> String? {
        guard let target = item.symlinkTarget else { return nil }
        return target.hasPrefix("/") ? WorkspaceFileService.shownPath(target) : target
    }

    static func rows(from items: [WorkspaceFileItem], showHidden: Bool, mode: WorkspaceFileSortMode) -> [WorkspaceFileRow] {
        let visible = showHidden ? items : items.filter { $0.name.utf8.first != UInt8(ascii: ".") }
        let sorted = visible.sorted { a, b in
            switch mode {
            case .name:
                break
            case .size:
                if a.size != b.size { return a.size > b.size }
            case .date:
                let dateA = a.modified ?? .distantPast
                let dateB = b.modified ?? .distantPast
                if dateA != dateB { return dateA > dateB }
            case .kind:
                if a.kind != b.kind { return a.kind.sortRank < b.kind.sortRank }
            }
            return a.name.compare(b.name, options: .caseInsensitive) == .orderedAscending
        }
        return sorted.map { WorkspaceFileRow(item: $0, linkTarget: shownLinkTarget($0)) }
    }

    static func sameDisplay(_ first: [WorkspaceFileRow], _ second: [WorkspaceFileRow]) -> Bool {
        guard first.count == second.count else { return false }
        for (a, b) in zip(first, second) {
            if a.item.path != b.item.path || a.item.kind != b.item.kind || a.item.size != b.item.size
                || a.item.modified != b.item.modified || a.item.posixMode != b.item.posixMode
                || a.linkTarget != b.linkTarget {
                return false
            }
        }
        return true
    }

    /// No NUL in the first 8 KiB. Reads those itself because the service's capped read fails
    /// on anything larger than the cap, which would make every long text file binary.
    static func looksTextual(_ path: String) -> Bool {
        let descriptor = open(WorkspaceFileService.hostPath(path), O_RDONLY | O_NONBLOCK)
        guard descriptor >= 0 else { return false }
        defer { close(descriptor) }
        let capacity = 8192
        var buffer = [UInt8](repeating: 0, count: capacity)
        let count = read(descriptor, &buffer, capacity)
        guard count >= 0 else { return false }
        return !buffer[0..<count].contains(0)
    }

    static func owner(ofPath path: String) -> (uid: uid_t, gid: gid_t)? {
        let host = WorkspaceFileService.hostPath(path)
        var info = Darwin.stat()
        if stat(host, &info) == 0 {
            return (info.st_uid, info.st_gid)
        }
        if lstat(host, &info) == 0 {
            return (info.st_uid, info.st_gid)
        }
        return nil
    }
}

/// A Finder-modeled file manager: a sidebar of places, a native-scrolling list, a breadcrumb
/// with back, forward and up, and New Folder, Rename, Duplicate, Delete and Get Info.
///
/// Paths are as the shell sees them. Everything that touches the disk goes through
/// `WorkspaceFileService` or `WorkspaceFileManagerWork`, off the main thread.
@MainActor
final class WorkspaceFileManagerAppletViewController: WorkspaceAppletViewController, WorkspaceTextScalable,
    WorkspaceStatefulTool, WorkspaceFileOpenable, WorkspaceFocusable, UITableViewDataSource, UITableViewDelegate {
    private static let sidebarWidth: CGFloat = 180
    private static let sidebarCollapseThreshold: CGFloat = 520
    private static let toolbarHeight: CGFloat = 44
    private static let statusBarHeight: CGFloat = 28
    private static let sidebarRowHeight: CGFloat = 36
    // A fixed height, so a big folder's content size takes no per-row measuring.
    private static let fileRowHeight: CGFloat = 52
    private static let dividerThickness: CGFloat = 1.0 / 3.0

    private static let sortModeKey = "com.pscal.workspace.fileManager.sortMode"
    private static let showHiddenKey = "com.pscal.workspace.fileManager.showHidden"
    private static let fileCellID = "filemanager.row"
    private static let sidebarCellID = "filemanager.sidebar"

    private static let sizeFormatter: ByteCountFormatter = {
        let formatter = ByteCountFormatter()
        formatter.countStyle = .file
        return formatter
    }()
    private static let dateFormatter: DateFormatter = {
        let formatter = DateFormatter()
        formatter.dateStyle = .short
        formatter.timeStyle = .short
        return formatter
    }()

    private var sidebarContainer: UIView!
    private var sidebarTable: UITableView!
    private var sidebarWidthConstraint: NSLayoutConstraint!
    private var sidebarHidden = false
    private var dividerView: UIView!

    private var toolbarView: UIView!
    private var toolbarHeightConstraint: NSLayoutConstraint!
    private var pathScrollHeightConstraint: NSLayoutConstraint!
    private var statusBarHeightConstraint: NSLayoutConstraint!
    private var backButton: UIButton!
    private var forwardButton: UIButton!
    private var upButton: UIButton!
    private var moreButton: UIButton!
    private var pathScrollView: UIScrollView!  // breadcrumb: one tappable button per component
    private var pathStack: UIStackView!

    private var fileTable: UITableView!
    private var statusBarView: UIView!
    private var statusLabel: UILabel!

    /// Empty until home has been worked out, or something asks for a folder first.
    private(set) var currentPath = ""
    private var backHistory: [String] = []
    private var forwardHistory: [String] = []
    private var places: [WorkspaceFilePlace] = []

    private var allItems: [WorkspaceFileItem] = []  // last listing, unfiltered
    private var rows: [WorkspaceFileRow] = []       // filtered and sorted, what the table shows
    private var loadError: Error?
    private(set) var isLoading = false
    private var loadGeneration = 0                  // discards stale listings
    // ...and this STOPS a superseded one. The generation only throws the answer away; the
    // enumeration would keep running and hold the lane.
    private var listOperation: WorkspaceFileOperation?
    private var arrangeGeneration = 0
    private var availableBytes: Int64 = 0           // 0 means no figure, not full
    private var lastListingFinished: CFTimeInterval = 0

    private var sortMode = WorkspaceFileSortMode.name
    private var showHidden = false
    private var navigationPinned = false  // an open is on its way; the start at home must not override it

    // The file rows' fonts are UIKit's subtitle-cell defaults. These are those defaults, read
    // off a fresh cell, and whether any row has been given a scaled font since.
    private var fileRowBaseTextFont: UIFont?
    private var fileRowBaseDetailFont: UIFont?
    private var fileRowFontsScaled = false

    /// The names listed, in order, for tests.
    var shownNames: [String] { rows.map(\.item.name) }

    deinit {
        listOperation?.cancel()
    }

    // MARK: - Lifecycle

    override func viewDidLoad() {
        super.viewDidLoad()
        title = "File Manager"

        let defaults = UserDefaults.standard
        sortMode = WorkspaceFileSortMode(rawValue: defaults.integer(forKey: Self.sortModeKey)) ?? .name
        showHidden = defaults.bool(forKey: Self.showHiddenKey)

        buildSidebar()
        buildDivider()
        buildToolbar()
        buildFileTable()
        buildStatusBar()
        activateRegionConstraints()

        refreshMoreMenu()
        updateToolbarState()
        applyTheme()
        if currentPath.isEmpty {
            setLoading(true)
        } else {
            reload()
        }
        reloadPlaces()
    }

    override func viewWillAppear(_ animated: Bool) {
        super.viewWillAppear(animated)
        reloadPlaces()
    }

    override func viewDidLayoutSubviews() {
        super.viewDidLayoutSubviews()
        let hide = view.bounds.width < Self.sidebarCollapseThreshold
        guard sidebarContainer != nil, hide != sidebarHidden else { return }
        sidebarHidden = hide
        sidebarContainer.isHidden = hide
        sidebarWidthConstraint.constant = hide ? 0 : Self.sidebarWidth
    }

    /// Asks where the fixed places are and what's been mounted, and starts at home if nothing
    /// has asked for a folder in the meantime.
    private func reloadPlaces() {
        guard fileTable != nil else { return }
        WorkspaceFileManagerWork.run({ WorkspaceFileManagerWork.places() }, completion: { [weak self] found in
            guard let self else { return }
            if found != self.places {
                self.places = found
                self.sidebarTable.reloadData()
                self.refreshMoreMenu()
            }
            if self.currentPath.isEmpty, !self.navigationPinned, let home = found.first {
                self.show(path: home.path, pushHistory: false)
            }
        })
    }

    // MARK: - WorkspaceStatefulTool

    func workspaceToolStateForSaving() -> [String: Any]? {
        currentPath.isEmpty ? nil : ["path": currentPath]
    }

    func workspaceRestoreToolState(_ state: [String: Any]) {
        guard let path = state["path"] as? String, !path.isEmpty else { return }
        // A path that no longer exists just shows the listing's error.
        show(path: Self.withoutTrailingSlashes(path), pushHistory: false)
    }

    // MARK: - WorkspaceFileOpenable

    /// Opening a folder shows it; opening a file reveals the folder holding it.
    func workspaceOpenFile(atPath path: String) {
        navigationPinned = true
        let path = Self.withoutTrailingSlashes(path)
        WorkspaceFileService.shared.item(path) { [weak self] result in
            guard let self else { return }
            if case .success(let item) = result, item.kind == .directory {
                self.navigate(toPath: item.path)
            } else {
                self.navigate(toPath: WorkspaceFileService.parent(of: path))
            }
        }
    }

    private static func withoutTrailingSlashes(_ path: String) -> String {
        var trimmed = path
        while trimmed.count > 1 && trimmed.hasSuffix("/") {
            trimmed.removeLast()
        }
        return trimmed
    }

    // MARK: - WorkspaceFocusable

    /// Key commands are collected along the responder chain, and this applet has no text input
    /// to anchor it there, so the controller itself takes first responder and yields when
    /// a terminal or an editor is focused. This also fires on any tap in the window, so a
    /// refresh of what a shell beside it may have changed waits until the list has stood a
    /// moment, and shows only if something did change.
    func workspaceToolDidBecomeFrontmost() {
        becomeFirstResponder()
        reloadPlaces()
        if !currentPath.isEmpty, listOperation == nil, CACurrentMediaTime() - lastListingFinished > 2 {
            reload(showingProgress: false)
        }
    }

    // MARK: - Hardware keyboard

    override var canBecomeFirstResponder: Bool { true }

    // Only shortcuts that don't need a selected row: tapping opens, so there's no persistent
    // selection to act on.
    override var keyCommands: [UIKeyCommand]? {
        let commands = [
            UIKeyCommand(title: "Enclosing Folder", action: #selector(navigateUp), input: UIKeyCommand.inputUpArrow, modifierFlags: .command),
            UIKeyCommand(title: "Back", action: #selector(navigateBack), input: "[", modifierFlags: .command),
            UIKeyCommand(title: "Forward", action: #selector(navigateForward), input: "]", modifierFlags: .command),
            UIKeyCommand(title: "Show/Hide Hidden Files", action: #selector(toggleHiddenFiles), input: ".", modifierFlags: [.command, .shift]),
            UIKeyCommand(title: "Refresh", action: #selector(refresh), input: "r", modifierFlags: .command),
            UIKeyCommand(title: "New File Manager Window", action: #selector(openNewWindowHere), input: "n", modifierFlags: .command),
        ]
        commands.forEach { $0.wantsPriorityOverSystemBehavior = true }
        return commands
    }

    @objc private func openNewWindowHere() {
        guard !currentPath.isEmpty else { return }
        workspaceHost?.open(tool: WorkspaceToolRegistry.fileManager, path: currentPath)
    }

    @objc private func toggleHiddenFiles() {
        showHidden.toggle()
        UserDefaults.standard.set(showHidden, forKey: Self.showHiddenKey)
        applyFilterAndSort()
        refreshMoreMenu()
    }

    @objc func refresh() {
        reload()
    }

    // MARK: - Building

    private func buildSidebar() {
        sidebarContainer = UIView()
        sidebarContainer.translatesAutoresizingMaskIntoConstraints = false
        sidebarContainer.clipsToBounds = true
        toolContentView.addSubview(sidebarContainer)

        sidebarTable = UITableView(frame: .zero, style: .plain)
        sidebarTable.translatesAutoresizingMaskIntoConstraints = false
        sidebarTable.dataSource = self
        sidebarTable.delegate = self
        sidebarTable.rowHeight = scaledHeight(Self.sidebarRowHeight)
        sidebarTable.accessibilityIdentifier = "workspace.filemanager.sidebar"
        sidebarContainer.addSubview(sidebarTable)

        NSLayoutConstraint.activate([
            sidebarTable.topAnchor.constraint(equalTo: sidebarContainer.topAnchor),
            sidebarTable.leadingAnchor.constraint(equalTo: sidebarContainer.leadingAnchor),
            sidebarTable.trailingAnchor.constraint(equalTo: sidebarContainer.trailingAnchor),
            sidebarTable.bottomAnchor.constraint(equalTo: sidebarContainer.bottomAnchor),
        ])
    }

    private func buildDivider() {
        dividerView = UIView()
        dividerView.translatesAutoresizingMaskIntoConstraints = false
        toolContentView.addSubview(dividerView)
    }

    private func buildToolbar() {
        toolbarView = UIView()
        toolbarView.translatesAutoresizingMaskIntoConstraints = false
        toolContentView.addSubview(toolbarView)

        backButton = toolbarIconButton(symbol: "chevron.left", action: #selector(navigateBack))
        backButton.accessibilityLabel = "Back"
        backButton.accessibilityHint = "Navigates to the previously visited folder."
        backButton.accessibilityIdentifier = "workspace.filemanager.back"
        forwardButton = toolbarIconButton(symbol: "chevron.right", action: #selector(navigateForward))
        forwardButton.accessibilityLabel = "Forward"
        forwardButton.accessibilityHint = "Navigates forward in your folder history."
        forwardButton.accessibilityIdentifier = "workspace.filemanager.forward"
        upButton = toolbarIconButton(symbol: "arrow.up", action: #selector(navigateUp))
        upButton.accessibilityLabel = "Up"
        upButton.accessibilityHint = "Navigates to the parent folder."
        upButton.accessibilityIdentifier = "workspace.filemanager.up"
        moreButton = toolbarIconButton(symbol: "ellipsis.circle", action: nil)
        moreButton.accessibilityLabel = "More Actions"
        moreButton.accessibilityHint = "Shows additional file manager options."
        moreButton.accessibilityIdentifier = "workspace.filemanager.more"
        moreButton.showsMenuAsPrimaryAction = true

        // One button per path component inside a horizontal scroll view, so deep paths scroll
        // rather than squeezing the navigation buttons out.
        pathScrollView = UIScrollView()
        pathScrollView.translatesAutoresizingMaskIntoConstraints = false
        pathScrollView.showsHorizontalScrollIndicator = false
        pathScrollView.showsVerticalScrollIndicator = false
        pathScrollView.accessibilityIdentifier = "workspace.filemanager.path"
        pathScrollView.setContentHuggingPriority(.defaultLow, for: .horizontal)
        pathScrollView.setContentCompressionResistancePriority(.defaultLow, for: .horizontal)

        pathStack = UIStackView()
        pathStack.translatesAutoresizingMaskIntoConstraints = false
        pathStack.axis = .horizontal
        pathStack.alignment = .center
        pathStack.spacing = 2
        pathScrollView.addSubview(pathStack)

        pathScrollHeightConstraint = pathScrollView.heightAnchor.constraint(equalToConstant: pathScrollHeight)
        NSLayoutConstraint.activate([
            pathStack.leadingAnchor.constraint(equalTo: pathScrollView.contentLayoutGuide.leadingAnchor),
            pathStack.trailingAnchor.constraint(equalTo: pathScrollView.contentLayoutGuide.trailingAnchor),
            pathStack.topAnchor.constraint(equalTo: pathScrollView.contentLayoutGuide.topAnchor),
            pathStack.bottomAnchor.constraint(equalTo: pathScrollView.contentLayoutGuide.bottomAnchor),
            pathStack.heightAnchor.constraint(equalTo: pathScrollView.frameLayoutGuide.heightAnchor),
            pathScrollHeightConstraint,
        ])

        let stack = UIStackView(arrangedSubviews: [backButton, forwardButton, upButton, pathScrollView, moreButton])
        stack.translatesAutoresizingMaskIntoConstraints = false
        stack.axis = .horizontal
        stack.alignment = .center
        stack.spacing = 10
        toolbarView.addSubview(stack)

        NSLayoutConstraint.activate([
            stack.leadingAnchor.constraint(equalTo: toolbarView.leadingAnchor, constant: 10),
            stack.trailingAnchor.constraint(equalTo: toolbarView.trailingAnchor, constant: -10),
            stack.centerYAnchor.constraint(equalTo: toolbarView.centerYAnchor),
        ])
    }

    private func toolbarIconButton(symbol: String, action: Selector?) -> UIButton {
        let button = UIButton(type: .system)
        button.translatesAutoresizingMaskIntoConstraints = false
        button.setImage(UIImage(systemName: symbol), for: .normal)
        if let action {
            button.addTarget(self, action: action, for: .touchUpInside)
        }
        button.widthAnchor.constraint(greaterThanOrEqualToConstant: 28).isActive = true
        return button
    }

    /// One tappable button per component, "/" and then each folder, the last rendered as the
    /// current location.
    private func rebuildBreadcrumb() {
        guard pathStack != nil else { return }
        clearArrangedSubviews(of: pathStack)
        guard !currentPath.isEmpty else { return }

        let colors = theme
        var prefixes = ["/"]
        var titles = ["/"]
        var running = ""
        for component in (currentPath as NSString).pathComponents where component != "/" {
            running += "/" + component
            prefixes.append(running)
            titles.append(component)
        }

        for index in prefixes.indices {
            let isLast = index == prefixes.count - 1
            if index > 0 {
                let separator = UILabel()
                separator.text = "›"
                separator.font = .systemFont(ofSize: toolScaledFontSize(12))
                separator.textColor = colors.secondary
                separator.isAccessibilityElement = false
                pathStack.addArrangedSubview(separator)
            }
            let button = UIButton(type: .system)
            button.titleLabel?.font = .systemFont(ofSize: toolScaledFontSize(13), weight: isLast ? .semibold : .regular)
            button.setTitle(titles[index], for: .normal)
            if titles[index] == "/" {
                button.accessibilityLabel = "Root Directory"
            }
            let color = isLast ? colors.primary : colors.accent
            button.setTitleColor(color, for: .normal)
            button.setTitleColor(color, for: .disabled)
            button.isEnabled = !isLast  // the current location isn't a link anywhere
            button.accessibilityHint = isLast ? nil : "Go to this folder"
            button.accessibilityIdentifier = "workspace.filemanager.crumb.\(index)"
            let target = prefixes[index]
            button.addAction(UIAction { [weak self] _ in
                self?.navigate(toPath: target)
            }, for: .touchUpInside)
            pathStack.addArrangedSubview(button)
        }

        // Deep paths overflow leftward; keep the current location in view.
        pathScrollView.layoutIfNeeded()
        let overflow = pathStack.bounds.width - pathScrollView.bounds.width
        pathScrollView.setContentOffset(CGPoint(x: max(0, overflow), y: 0), animated: false)
    }

    private func buildFileTable() {
        fileTable = UITableView(frame: .zero, style: .plain)
        fileTable.translatesAutoresizingMaskIntoConstraints = false
        fileTable.dataSource = self
        fileTable.delegate = self
        fileTable.backgroundColor = .clear
        fileTable.rowHeight = scaledHeight(Self.fileRowHeight)
        fileTable.estimatedRowHeight = 0
        fileTable.accessibilityIdentifier = "workspace.filemanager.list"
        toolContentView.addSubview(fileTable)
    }

    private func buildStatusBar() {
        statusBarView = UIView()
        statusBarView.translatesAutoresizingMaskIntoConstraints = false
        toolContentView.addSubview(statusBarView)

        statusLabel = UILabel()
        statusLabel.translatesAutoresizingMaskIntoConstraints = false
        statusLabel.font = .systemFont(ofSize: toolScaledFontSize(11))
        statusLabel.accessibilityIdentifier = "workspace.filemanager.status"
        statusBarView.addSubview(statusLabel)

        NSLayoutConstraint.activate([
            statusLabel.leadingAnchor.constraint(equalTo: statusBarView.leadingAnchor, constant: 10),
            statusLabel.trailingAnchor.constraint(lessThanOrEqualTo: statusBarView.trailingAnchor, constant: -10),
            statusLabel.centerYAnchor.constraint(equalTo: statusBarView.centerYAnchor),
        ])
    }

    // Explicit anchors rather than a filling stack view: a UITableView has no reliable
    // intrinsic size, so every region is pinned by hand.
    private func activateRegionConstraints() {
        let content = toolContentView
        sidebarWidthConstraint = sidebarContainer.widthAnchor.constraint(equalToConstant: Self.sidebarWidth)
        toolbarHeightConstraint = toolbarView.heightAnchor.constraint(equalToConstant: pathScrollHeight + 12)
        statusBarHeightConstraint = statusBarView.heightAnchor.constraint(equalToConstant: scaledHeight(Self.statusBarHeight))
        NSLayoutConstraint.activate([
            sidebarContainer.topAnchor.constraint(equalTo: content.topAnchor),
            sidebarContainer.leadingAnchor.constraint(equalTo: content.leadingAnchor),
            sidebarContainer.bottomAnchor.constraint(equalTo: content.bottomAnchor),
            sidebarWidthConstraint,

            dividerView.topAnchor.constraint(equalTo: content.topAnchor),
            dividerView.bottomAnchor.constraint(equalTo: content.bottomAnchor),
            dividerView.leadingAnchor.constraint(equalTo: sidebarContainer.trailingAnchor),
            dividerView.widthAnchor.constraint(equalToConstant: Self.dividerThickness),

            toolbarView.topAnchor.constraint(equalTo: content.topAnchor),
            toolbarView.leadingAnchor.constraint(equalTo: dividerView.trailingAnchor),
            toolbarView.trailingAnchor.constraint(equalTo: content.trailingAnchor),
            toolbarHeightConstraint,

            fileTable.topAnchor.constraint(equalTo: toolbarView.bottomAnchor),
            fileTable.leadingAnchor.constraint(equalTo: dividerView.trailingAnchor),
            fileTable.trailingAnchor.constraint(equalTo: content.trailingAnchor),
            fileTable.bottomAnchor.constraint(equalTo: statusBarView.topAnchor),

            statusBarView.leadingAnchor.constraint(equalTo: dividerView.trailingAnchor),
            statusBarView.trailingAnchor.constraint(equalTo: content.trailingAnchor),
            statusBarView.bottomAnchor.constraint(equalTo: content.bottomAnchor),
            statusBarHeightConstraint,
        ])
    }

    // MARK: - Text scale

    // What scales is what a person reads: file names and their size and date line, the
    // sidebar's place names, the breadcrumb and the status line. The toolbar's icons, the row
    // icons and the sidebar's width stay as they are. Each font is built from its unscaled
    // size, so a theme change or a reload keeps the scale rather than compounding it.
    override func applyTextScale() {
        super.applyTextScale()
        guard fileTable != nil else { return }
        if abs(workspaceTextScale - 1) > 0.001 {
            fileRowFontsScaled = true
        }
        fileTable.rowHeight = scaledHeight(Self.fileRowHeight)
        sidebarTable.rowHeight = scaledHeight(Self.sidebarRowHeight)
        pathScrollHeightConstraint.constant = pathScrollHeight
        toolbarHeightConstraint.constant = pathScrollHeight + 12
        statusBarHeightConstraint.constant = scaledHeight(Self.statusBarHeight)
        statusLabel.font = .systemFont(ofSize: toolScaledFontSize(11))
        rebuildBreadcrumb()
        fileTable.reloadData()
        sidebarTable.reloadData()
        updateEmptyState()
    }

    /// Whole points, and exactly `height` at scale 1.
    private func scaledHeight(_ height: CGFloat) -> CGFloat {
        (height * workspaceTextScale).rounded(.up)
    }

    // The breadcrumb grows with its text, and the toolbar with it. Neither shrinks below its
    // default: the buttons beside the path don't scale, and a smaller bar would crowd them.
    private var pathScrollHeight: CGFloat {
        let unscaled = Self.toolbarHeight - 12
        return max(unscaled, scaledHeight(unscaled))
    }

    // Until the text is first scaled a file row keeps UIKit's own fonts. After that every row
    // is given one, even back at 1, because a dequeued cell still carries its last row's font.
    private func applyTextScale(toFileCell cell: UITableViewCell) {
        guard fileRowFontsScaled, let base = fileRowBaseTextFont else { return }
        cell.textLabel?.font = base.withSize(toolScaledFontSize(base.pointSize))
        if let detail = fileRowBaseDetailFont {
            cell.detailTextLabel?.font = detail.withSize(toolScaledFontSize(detail.pointSize))
        }
    }

    // MARK: - Theme

    override func applyTheme() {
        super.applyTheme()
        // The base class calls this from its viewDidLoad, before ours has built anything.
        guard fileTable != nil else { return }
        let colors = theme
        // The theme's text colours are only sure to contrast with its card surfaces, never
        // with the gradient behind, so this applet sits on a card colour like the others.
        toolContentView.backgroundColor = colors.card.withAlphaComponent(0.95)
        rebuildBreadcrumb()  // its buttons bake theme colours at build time
        statusLabel.textColor = colors.secondary
        dividerView.backgroundColor = colors.stroke
        fileTable.separatorColor = colors.stroke
        sidebarTable.separatorColor = colors.stroke
        sidebarTable.backgroundColor = colors.cardAlt.withAlphaComponent(0.95)
        for button in [backButton, forwardButton, upButton, moreButton] {
            button?.tintColor = colors.accent
        }
        fileTable.reloadData()
        sidebarTable.reloadData()
        updateEmptyState()  // its label bakes theme colours at creation
    }

    // MARK: - Navigation

    func navigate(toPath path: String) {
        show(path: path, pushHistory: true)
    }

    private func show(path: String, pushHistory: Bool) {
        if pushHistory, !currentPath.isEmpty, currentPath != path {
            backHistory.append(currentPath)
            forwardHistory.removeAll()
        }
        arrive(at: path)
    }

    private func arrive(at path: String) {
        let changed = path != currentPath
        currentPath = path
        guard fileTable != nil else { return }  // viewDidLoad will list it
        updateToolbarState()
        if changed {
            // The last folder's rows mustn't sit under this one's breadcrumb.
            arrangeGeneration += 1
            allItems = []
            rows = []
            loadError = nil
            fileTable.reloadData()
            fileTable.setContentOffset(.zero, animated: false)
        }
        reload()
    }

    @objc private func navigateBack() {
        guard let target = backHistory.popLast() else { return }
        forwardHistory.append(currentPath)
        arrive(at: target)
    }

    @objc private func navigateForward() {
        guard let target = forwardHistory.popLast() else { return }
        backHistory.append(currentPath)
        arrive(at: target)
    }

    @objc private func navigateUp() {
        guard !currentPath.isEmpty, currentPath != "/" else { return }
        navigate(toPath: WorkspaceFileService.parent(of: currentPath))
    }

    private func updateToolbarState() {
        backButton.isEnabled = !backHistory.isEmpty
        forwardButton.isEnabled = !forwardHistory.isEmpty
        upButton.isEnabled = !currentPath.isEmpty && currentPath != "/"
        rebuildBreadcrumb()
    }

    // MARK: - Loading

    /// `showingProgress` is false for a refresh nobody asked for: it doesn't lock the list or
    /// show a spinner, and the table only reloads if something changed.
    private func reload(showingProgress: Bool = true) {
        guard !currentPath.isEmpty, fileTable != nil else { return }
        loadGeneration += 1
        let generation = loadGeneration
        listOperation?.cancel()
        listOperation = nil
        if showingProgress {
            setLoading(true)
        }
        let path = currentPath
        listOperation = WorkspaceFileService.shared.listDirectory(path) { [weak self] result in
            guard let self, self.loadGeneration == generation else { return }  // a newer one superseded it
            self.listOperation = nil
            self.lastListingFinished = CACurrentMediaTime()
            switch result {
            case .success(let items):
                self.allItems = items
                self.loadError = nil
            case .failure(let error):
                self.allItems = []
                self.loadError = error
            }
            self.applyFilterAndSort()
        }
        WorkspaceFileService.shared.filesystemStatus(path) { [weak self] result in
            guard let self, self.loadGeneration == generation else { return }
            if case .success(let status) = result {
                self.availableBytes = status.available
            } else {
                self.availableBytes = 0
            }
            self.updateStatusLabel()
        }
    }

    private func setLoading(_ loading: Bool) {
        isLoading = loading
        fileTable.isUserInteractionEnabled = !loading
        if loading {
            let spinner = UIActivityIndicatorView(style: .medium)
            spinner.color = theme.secondary
            spinner.accessibilityIdentifier = "workspace.filemanager.loading"
            spinner.startAnimating()
            fileTable.backgroundView = spinner
        } else {
            updateEmptyState()
        }
    }

    /// Off the main thread, because sorting ten thousand names by a case-insensitive compare is
    /// long enough to feel. The newest arrangement wins.
    private func applyFilterAndSort() {
        arrangeGeneration += 1
        let generation = arrangeGeneration
        let items = allItems
        let showHidden = showHidden
        let mode = sortMode
        let previous = rows
        WorkspaceFileManagerWork.run({ () -> (rows: [WorkspaceFileRow], changed: Bool) in
            let arranged = WorkspaceFileManagerWork.rows(from: items, showHidden: showHidden, mode: mode)
            return (arranged, !WorkspaceFileManagerWork.sameDisplay(arranged, previous))
        }, completion: { [weak self] result in
            guard let self, self.arrangeGeneration == generation else { return }
            self.rows = result.rows
            if result.changed {
                self.fileTable.reloadData()
            }
            self.setLoading(self.listOperation != nil || self.currentPath.isEmpty)
            self.updateStatusLabel()
        })
    }

    private func updateEmptyState() {
        guard !isLoading else { return }
        guard rows.isEmpty else {
            fileTable.backgroundView = nil
            return
        }
        let label = UILabel()
        label.textAlignment = .center
        label.numberOfLines = 0
        label.textColor = theme.secondary
        label.font = .systemFont(ofSize: toolScaledFontSize(15))
        label.accessibilityIdentifier = "workspace.filemanager.empty"
        if let loadError {
            let description = loadError.localizedDescription
            label.text = description.isEmpty ? "Couldn’t load this folder." : description
        } else {
            label.text = "This folder is empty."
        }
        fileTable.backgroundView = label
    }

    private func updateStatusLabel() {
        var text = rows.count == 1 ? "1 item" : "\(rows.count) items"
        // 0 means the filesystem reports no figure, not that it's full.
        if availableBytes > 0 {
            text += " · \(Self.formattedSize(UInt64(availableBytes))) available"
        }
        statusLabel.text = text
    }

    // MARK: - UITableViewDataSource

    func tableView(_ tableView: UITableView, numberOfRowsInSection section: Int) -> Int {
        tableView === sidebarTable ? places.count : rows.count
    }

    func tableView(_ tableView: UITableView, cellForRowAt indexPath: IndexPath) -> UITableViewCell {
        tableView === sidebarTable ? sidebarCell(at: indexPath) : fileCell(at: indexPath)
    }

    private func sidebarCell(at indexPath: IndexPath) -> UITableViewCell {
        let cell = sidebarTable.dequeueReusableCell(withIdentifier: Self.sidebarCellID)
            ?? UITableViewCell(style: .default, reuseIdentifier: Self.sidebarCellID)
        guard indexPath.row < places.count else { return cell }
        let place = places[indexPath.row]
        let colors = theme
        cell.textLabel?.text = place.title
        cell.textLabel?.font = .systemFont(ofSize: toolScaledFontSize(13))
        cell.textLabel?.textColor = colors.primary
        cell.textLabel?.lineBreakMode = .byTruncatingMiddle
        cell.imageView?.image = UIImage(systemName: place.symbol)
        cell.imageView?.tintColor = colors.accent
        cell.backgroundColor = .clear
        cell.accessibilityIdentifier = "workspace.filemanager.place.\(place.path)"
        return cell
    }

    private func fileCell(at indexPath: IndexPath) -> UITableViewCell {
        let cell: UITableViewCell
        if let reused = fileTable.dequeueReusableCell(withIdentifier: Self.fileCellID) {
            cell = reused
        } else {
            cell = UITableViewCell(style: .subtitle, reuseIdentifier: Self.fileCellID)
            // A new cell has had nothing set on it, so its fonts are the defaults the text
            // scale multiplies.
            if fileRowBaseTextFont == nil {
                fileRowBaseTextFont = cell.textLabel?.font
                fileRowBaseDetailFont = cell.detailTextLabel?.font
            }
        }
        applyTextScale(toFileCell: cell)
        guard indexPath.row < rows.count else { return cell }

        let row = rows[indexPath.row]
        let item = row.item
        let colors = theme
        let openable = Self.isOpenable(item)
        cell.textLabel?.text = item.name
        cell.textLabel?.textColor = openable ? colors.primary : colors.secondary
        cell.detailTextLabel?.text = Self.subtitle(for: row)
        cell.detailTextLabel?.textColor = colors.secondary
        cell.imageView?.image = Self.icon(for: item)
        cell.imageView?.tintColor = colors.accent
        cell.backgroundColor = .clear  // a cell defaults to opaque white, which hides a theme's light text
        cell.accessoryType = item.kind == .directory ? .disclosureIndicator : .none
        cell.selectionStyle = openable ? .default : .none
        cell.accessibilityIdentifier = "workspace.filemanager.row.\(item.name)"
        return cell
    }

    // MARK: - UITableViewDelegate

    func tableView(_ tableView: UITableView, didSelectRowAt indexPath: IndexPath) {
        tableView.deselectRow(at: indexPath, animated: true)
        becomeFirstResponder()  // anchors the keyboard shortcuts here after work elsewhere
        if tableView === sidebarTable {
            if indexPath.row < places.count {
                navigate(toPath: places[indexPath.row].path)
            }
            return
        }
        guard indexPath.row < rows.count else { return }
        let item = rows[indexPath.row].item
        guard Self.isOpenable(item) else { return }
        activate(item)
    }

    func tableView(_ tableView: UITableView, contextMenuConfigurationForRowAt indexPath: IndexPath, point: CGPoint) -> UIContextMenuConfiguration? {
        guard tableView === fileTable, indexPath.row < rows.count else { return nil }
        let item = rows[indexPath.row].item
        return UIContextMenuConfiguration(identifier: nil, previewProvider: nil) { [weak self] _ in
            self?.contextMenu(for: item)
        }
    }

    func tableView(_ tableView: UITableView, trailingSwipeActionsConfigurationForRowAt indexPath: IndexPath) -> UISwipeActionsConfiguration? {
        guard tableView === fileTable, indexPath.row < rows.count else { return nil }
        let item = rows[indexPath.row].item
        let delete = UIContextualAction(style: .destructive, title: "Delete") { [weak self] _, _, done in
            self?.confirmDelete(item)
            done(true)  // dismiss the swipe now; the confirmation does the rest
        }
        return UISwipeActionsConfiguration(actions: [delete])
    }

    private func contextMenu(for item: WorkspaceFileItem) -> UIMenu {
        var actions: [UIMenuElement] = []
        if Self.isOpenable(item) {
            actions.append(UIAction(title: "Open", image: UIImage(systemName: "arrow.up.forward.square")) { [weak self] _ in
                self?.activate(item)
            })
        }
        if item.kind == .regular {
            actions.append(UIAction(title: "Share…", image: UIImage(systemName: "square.and.arrow.up")) { [weak self] _ in
                self?.share(item)
            })
            actions.append(UIAction(title: "Duplicate", image: UIImage(systemName: "plus.square.on.square")) { [weak self] _ in
                self?.duplicate(item)
            })
        }
        actions.append(UIAction(title: "Rename…", image: UIImage(systemName: "pencil")) { [weak self] _ in
            self?.promptRename(item)
        })
        actions.append(UIAction(title: "Get Info", image: UIImage(systemName: "info.circle")) { [weak self] _ in
            self?.presentInfo(for: item)
        })
        actions.append(UIAction(title: "Delete", image: UIImage(systemName: "trash"), attributes: .destructive) { [weak self] _ in
            self?.confirmDelete(item)
        })
        return UIMenu(title: "", children: actions)
    }

    // MARK: - Row presentation

    private static func isOpenable(_ item: WorkspaceFileItem) -> Bool {
        item.kind == .directory || item.kind == .regular
    }

    private static func icon(for item: WorkspaceFileItem) -> UIImage? {
        let symbol: String
        switch item.kind {
        case .directory:
            symbol = "folder"
        case .regular:
            switch WorkspaceFileOpenRouter.category(ofPath: item.name) {
            case .image: symbol = "photo"
            case .video: symbol = "film"
            case .audio: symbol = "music.note"
            default: symbol = "doc.text"
            }
        default:
            symbol = "questionmark.square"
        }
        return UIImage(systemName: symbol)
    }

    private static func subtitle(for row: WorkspaceFileRow) -> String? {
        let item = row.item
        if item.isBrokenSymlink {
            return "Broken alias → \(row.linkTarget ?? "?")"
        }
        if item.kind == .directory {
            return item.isSymlink ? "Alias → \(row.linkTarget ?? "?")" : nil
        }
        let size = formattedSize(item.size)
        guard let modified = item.modified else { return size }
        return "\(size) · \(dateFormatter.string(from: modified))"
    }

    private static func formattedSize(_ size: UInt64) -> String {
        sizeFormatter.string(fromByteCount: Int64(clamping: size))
    }

    private static func kindName(for item: WorkspaceFileItem) -> String {
        switch item.kind {
        case .directory: return "Folder"
        case .regular: return "File"
        case .symlink: return "Broken Alias"
        case .fifo: return "Named Pipe"
        case .socket: return "Socket"
        case .charDevice: return "Character Device"
        case .blockDevice: return "Block Device"
        case .other: return "Item"
        }
    }

    private static func formattedMode(_ mode: mode_t) -> String {
        let letters = Array("rwxrwxrwx")
        var text = ""
        for index in 0..<9 {
            let bit = mode_t(1) << mode_t(8 - index)
            text.append(mode & bit != 0 ? letters[index] : "-")
        }
        return "\(text) (\(String(mode & 0o7777, radix: 8)))"
    }

    // MARK: - Opening

    private func activate(_ item: WorkspaceFileItem) {
        if item.kind == .directory {
            navigate(toPath: item.path)
        } else {
            openFile(item)
        }
    }

    /// The row may be out of date, so this looks again first: a file deleted while it was
    /// shown gets a message and a fresh list rather than an applet with nothing to show.
    private func openFile(_ item: WorkspaceFileItem) {
        WorkspaceFileService.shared.item(item.path) { [weak self] result in
            guard let self else { return }
            switch result {
            case .failure(let error):
                self.presentAlert(title: "Can’t Open", message: "“\(item.name)”: \(error.localizedDescription)")
                self.reload(showingProgress: false)
            case .success(let current):
                self.route(current)
            }
        }
    }

    private func route(_ item: WorkspaceFileItem) {
        if item.kind == .directory {
            navigate(toPath: item.path)
            return
        }
        // A regular file with any execute bit is a program, not a document: run it in a new
        // terminal, like double-clicking one on a desktop, since no viewer can run it.
        if item.kind == .regular, item.posixMode & 0o111 != 0 {
            workspaceHost?.openNewTerminalWindow(runningOnce: Self.shellQuoted(item.path), title: item.name)
            return
        }
        guard let tool = WorkspaceFileOpenRouter.tool(forPath: item.path, kind: item.kind) else {
            presentNoViewer(for: item)
            return
        }
        guard WorkspaceFileOpenRouter.dependsOnContent(path: item.path) else {
            open(item, in: tool)
            return
        }
        let path = item.path
        WorkspaceFileManagerWork.run({ WorkspaceFileManagerWork.looksTextual(path) }, completion: { [weak self] textual in
            guard let self else { return }
            if textual {
                self.open(item, in: tool)
            } else {
                self.presentNoViewer(for: item)
            }
        })
    }

    private func open(_ item: WorkspaceFileItem, in tool: String) {
        guard WorkspaceToolRegistry.isAvailable(tool), let host = workspaceHost else {
            presentNoViewer(for: item)
            return
        }
        host.open(tool: tool, path: item.path)
    }

    private func presentNoViewer(for item: WorkspaceFileItem) {
        let alert = UIAlertController(title: "Can’t Open", message: "There’s no app to open “\(item.name)” yet.", preferredStyle: .alert)
        if item.kind == .regular {
            alert.addAction(UIAlertAction(title: "Share…", style: .default) { [weak self] _ in
                self?.share(item)
            })
        }
        alert.addAction(UIAlertAction(title: "OK", style: .cancel))
        presentModally(alert)
    }

    /// The system's sheet, which holds Open In as well as Share. The file is already a real
    /// one, so there's nothing to copy out first.
    private func share(_ item: WorkspaceFileItem) {
        let path = item.path
        WorkspaceFileManagerWork.run({ () -> URL? in
            let url = WorkspaceFileService.fileURL(path)
            return access(url.path, R_OK) == 0 ? url : nil
        }, completion: { [weak self] url in
            guard let self else { return }
            guard let url else {
                self.presentAlert(title: "Can’t Share", message: "“\(item.name)” can’t be read. It may have been deleted.")
                self.reload(showingProgress: false)
                return
            }
            let controller = UIActivityViewController(activityItems: [url], applicationActivities: nil)
            if let popover = controller.popoverPresentationController {
                let bounds = self.toolContentView.bounds
                popover.sourceView = self.toolContentView
                popover.sourceRect = CGRect(x: bounds.midX, y: bounds.midY, width: 0, height: 0)
                popover.permittedArrowDirections = []
            }
            self.presentModally(controller)
        })
    }

    /// One shell word, whatever the path holds: foo'bar becomes 'foo'\''bar'.
    private static func shellQuoted(_ path: String) -> String {
        "'" + path.replacingOccurrences(of: "'", with: "'\\''") + "'"
    }

    // MARK: - File operations

    private static func isValidName(_ name: String) -> Bool {
        !name.isEmpty && !name.contains("/") && !name.contains("\0") && name != "." && name != ".."
    }

    private static func enteredName(in alert: UIAlertController?) -> String {
        (alert?.textFields?.first?.text ?? "").trimmingCharacters(in: .whitespaces)
    }

    private func presentInvalidName() {
        presentAlert(title: "Invalid Name", message: "That name isn’t valid.")
    }

    /// Reloads either way. After a failure that's because what failed may be gone: a row
    /// deleted from a shell, say.
    private func finish(_ result: Result<Void, Error>, failureTitle: String) {
        switch result {
        case .success:
            reload()
        case .failure(let error):
            presentAlert(title: failureTitle, message: error.localizedDescription)
            reload(showingProgress: false)
        }
    }

    /// A mount point's path leads to the folder it shows, so renaming or deleting it would
    /// move or destroy that folder rather than take it away from here.
    private func unlessMountPoint(_ item: WorkspaceFileItem, title: String, perform: @escaping @MainActor () -> Void) {
        let path = item.path
        WorkspaceFileManagerWork.run({ WorkspaceFileService.mountTargets().contains(path) }, completion: { [weak self] mounted in
            guard let self else { return }
            if mounted {
                self.presentAlert(title: title, message: "“\(item.name)” is a mounted folder. Use umount to take it away from here.")
            } else {
                perform()
            }
        })
    }

    private func promptNewFolder() {
        guard !currentPath.isEmpty else { return }
        let directory = currentPath
        let alert = UIAlertController(title: "New Folder", message: nil, preferredStyle: .alert)
        alert.addTextField { field in
            field.text = "untitled folder"
            field.placeholder = "Name"
            field.autocapitalizationType = .none
            field.autocorrectionType = .no
        }
        alert.addAction(UIAlertAction(title: "Cancel", style: .cancel))
        alert.addAction(UIAlertAction(title: "Create", style: .default) { [weak self, weak alert] _ in
            guard let self else { return }
            let name = Self.enteredName(in: alert)
            guard Self.isValidName(name) else {
                self.presentInvalidName()
                return
            }
            WorkspaceFileService.shared.createDirectory(WorkspaceFileService.join(directory, name)) { [weak self] result in
                self?.finish(result, failureTitle: "Couldn’t Create Folder")
            }
        })
        presentModally(alert)
    }

    private func promptRename(_ item: WorkspaceFileItem) {
        unlessMountPoint(item, title: "Can’t Rename") { [weak self] in
            self?.presentRenamePrompt(item)
        }
    }

    private func presentRenamePrompt(_ item: WorkspaceFileItem) {
        let alert = UIAlertController(title: "Rename", message: nil, preferredStyle: .alert)
        alert.addTextField { field in
            field.text = item.name
            field.placeholder = "Name"
            field.autocapitalizationType = .none
            field.autocorrectionType = .no
        }
        alert.addAction(UIAlertAction(title: "Cancel", style: .cancel))
        alert.addAction(UIAlertAction(title: "Rename", style: .default) { [weak self, weak alert] _ in
            guard let self else { return }
            let name = Self.enteredName(in: alert)
            guard name != item.name else { return }
            guard Self.isValidName(name) else {
                self.presentInvalidName()
                return
            }
            let destination = WorkspaceFileService.join(WorkspaceFileService.parent(of: item.path), name)
            WorkspaceFileService.shared.move(item.path, to: destination) { [weak self] result in
                self?.finish(result, failureTitle: "Couldn’t Rename")
            }
        })
        presentModally(alert)
    }

    private func duplicateName(for item: WorkspaceFileItem) -> String {
        let name = item.name as NSString
        let base = name.deletingPathExtension
        let ext = name.pathExtension
        let existing = Set(allItems.map(\.name))
        func withExtension(_ stem: String) -> String {
            ext.isEmpty ? stem : stem + "." + ext
        }
        var candidate = withExtension(base + " copy")
        var suffix = 2
        while existing.contains(candidate) {
            candidate = withExtension("\(base) copy \(suffix)")
            suffix += 1
        }
        return candidate
    }

    private func duplicate(_ item: WorkspaceFileItem) {
        let destination = WorkspaceFileService.join(WorkspaceFileService.parent(of: item.path), duplicateName(for: item))
        WorkspaceFileService.shared.copy(item.path, to: destination) { [weak self] result in
            self?.finish(result, failureTitle: "Couldn’t Duplicate")
        }
    }

    private func confirmDelete(_ item: WorkspaceFileItem) {
        unlessMountPoint(item, title: "Can’t Delete") { [weak self] in
            self?.presentDeleteConfirmation(item)
        }
    }

    private func presentDeleteConfirmation(_ item: WorkspaceFileItem) {
        let message: String
        if item.isSymlink {
            message = "This will delete the alias. What it points to stays."
        } else if item.kind == .directory {
            message = "This will permanently delete the folder and everything inside it."
        } else {
            message = "This will permanently delete the file."
        }
        let alert = UIAlertController(title: "Delete “\(item.name)”?", message: message, preferredStyle: .alert)
        alert.addAction(UIAlertAction(title: "Cancel", style: .cancel))
        alert.addAction(UIAlertAction(title: "Delete", style: .destructive) { [weak self] _ in
            WorkspaceFileService.shared.remove(item.path, recursive: true) { [weak self] result in
                self?.finish(result, failureTitle: "Couldn’t Delete")
            }
        })
        presentModally(alert)
    }

    /// Looks again for the owner, which the listing doesn't carry; a row whose file has gone
    /// still shows what was known of it.
    private func presentInfo(for item: WorkspaceFileItem) {
        let path = item.path
        WorkspaceFileManagerWork.run({ () -> (owner: (uid: uid_t, gid: gid_t)?, linkTarget: String?) in
            (WorkspaceFileManagerWork.owner(ofPath: path), WorkspaceFileManagerWork.shownLinkTarget(item))
        }, completion: { [weak self] found in
            guard let self else { return }
            var lines = ["Kind: \(Self.kindName(for: item))"]
            if item.kind != .directory {
                lines.append("Size: \(Self.formattedSize(item.size))")
            }
            if let modified = item.modified {
                lines.append("Modified: \(Self.dateFormatter.string(from: modified))")
            }
            lines.append("Permissions: \(Self.formattedMode(item.posixMode))")
            if let owner = found.owner {
                lines.append("Owner: \(owner.uid):\(owner.gid)")
            }
            if item.isSymlink {
                lines.append("\(item.isBrokenSymlink ? "Broken link" : "Alias") → \(found.linkTarget ?? "?")")
            }
            lines.append("Path: \(item.path)")
            self.presentAlert(title: item.name, message: lines.joined(separator: "\n"))
        })
    }

    private func presentAlert(title: String, message: String?) {
        let alert = UIAlertController(title: title, message: message?.isEmpty == false ? message : nil, preferredStyle: .alert)
        alert.addAction(UIAlertAction(title: "OK", style: .default))
        presentModally(alert)
    }

    /// On top of whatever is up, waiting out one that's still leaving: a refusal can come back
    /// while the confirmation before it is animating away, and presenting then drops it.
    private func presentModally(_ controller: UIViewController, attempt: Int = 0) {
        var top: UIViewController = workspaceHost ?? self
        while let presented = top.presentedViewController {
            top = presented
        }
        if (top.isBeingDismissed || top.isBeingPresented) && attempt < 8 {
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.15) { [weak self] in
                self?.presentModally(controller, attempt: attempt + 1)
            }
            return
        }
        top.present(controller, animated: true)
    }

    // MARK: - More menu

    private func refreshMoreMenu() {
        moreButton.menu = buildMoreMenu()
    }

    private func buildMoreMenu() -> UIMenu {
        let newFolder = UIAction(title: "New Folder", image: UIImage(systemName: "folder.badge.plus")) { [weak self] _ in
            self?.promptNewFolder()
        }
        // The sidebar collapses in a narrow window, which on a phone is every window, and it's
        // the only way to a mounted folder or back home.
        let goTo = UIMenu(title: "Go To", image: UIImage(systemName: "folder"), children: places.map { place in
            UIAction(title: place.title, image: UIImage(systemName: place.symbol)) { [weak self] _ in
                self?.navigate(toPath: place.path)
            }
        })
        let sortActions = WorkspaceFileSortMode.allCases.map { mode in
            UIAction(title: mode.title, state: sortMode == mode ? .on : .off) { [weak self] _ in
                self?.setSortMode(mode)
            }
        }
        let sortMenu = UIMenu(title: "Sort By", image: UIImage(systemName: "arrow.up.arrow.down"), children: sortActions)
        let hiddenToggle = UIAction(title: "Show Hidden Files", state: showHidden ? .on : .off) { [weak self] _ in
            self?.toggleHiddenFiles()
        }
        let refresh = UIAction(title: "Refresh", image: UIImage(systemName: "arrow.clockwise")) { [weak self] _ in
            self?.reload()
        }
        return UIMenu(title: "", children: [newFolder, goTo, sortMenu, hiddenToggle, refresh])
    }

    private func setSortMode(_ mode: WorkspaceFileSortMode) {
        sortMode = mode
        UserDefaults.standard.set(mode.rawValue, forKey: Self.sortModeKey)
        applyFilterAndSort()
        refreshMoreMenu()
    }
}
