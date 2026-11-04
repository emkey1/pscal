// Copyright 2026 Mike Miller
// SPDX-License-Identifier: Apache-2.0
//
// Ported by its author from iSH-AOK's Workspace (app/WorkspaceViewController.m at
// 0f75fee1c) and licensed to PSCAL under the Apache License 2.0.

import UIKit

// Where the app's disk has gone, ncdu-style: every place the app keeps data, largest first,
// and inside each the directories and files that make it up. A list that stopped at one total
// would say that 5 GB is gone and not where.
//
// A place is scanned once, off the main thread, into a table of what every directory under it
// holds; going into a directory after that only lists it. Sizes are what files occupy on the
// device (allocated blocks, a hard link counted once), which is what "storage used" means and
// can differ from the lengths `ls -l` shows.

private struct WorkspaceStorageUsage {
    var bytes: UInt64 = 0
    var files: UInt64 = 0
}

/// A row. Built off the main thread and only touched on it once handed over.
private final class WorkspaceStorageEntry: @unchecked Sendable {
    let name: String
    let hostPath: String
    var detail: String?
    var bytes: UInt64 = 0
    var files: UInt64 = 0
    var isDirectory = false
    var isSymlink = false
    /// Which place this is, across scans. Names needn't be unique.
    var placeKey: String?
    var scanning = false
    /// Relative directory path ("" for the place itself) to what it holds, once scanned.
    var tree: [String: WorkspaceStorageUsage]?
    /// So a place scanned twice keeps the newer answer.
    var scanSerial = 0

    init(name: String, hostPath: String) {
        self.name = name
        self.hostPath = hostPath
    }

    var isPlace: Bool { placeKey != nil }
    /// A place whose scan was cancelled has no sizes to show.
    var isUnmeasured: Bool { isPlace && !scanning && tree == nil }
    /// A file, and a place still being read or never measured, can't be gone into.
    var opens: Bool { isDirectory && !scanning && !isUnmeasured }
}

/// Walks everything under `path` and returns what each directory holds, keyed by its path
/// relative to `path`, with the whole of it, which is all there is when `path` is a file. nil
/// if cancelled. Symlinks aren't followed, so a loop or a link out of the tree counts as the
/// link alone, and neither is another filesystem entered.
private func workspaceStorageScan(path: String, operation: WorkspaceFileOperation) -> (tree: [String: WorkspaceStorageUsage], total: WorkspaceStorageUsage)? {
    var tree: [String: WorkspaceStorageUsage] = [:]
    var total = WorkspaceStorageUsage()
    guard let root = strdup(path) else { return (tree, total) }
    defer { free(root) }
    let rootLength = strlen(root)
    let roots: [UnsafeMutablePointer<CChar>?] = [root, nil]
    // FTS_NOCHDIR because a process with other threads can't change directory under them.
    guard let walk = fts_open(roots, FTS_PHYSICAL | FTS_NOCHDIR | FTS_XDEV, nil) else { return (tree, total) }
    defer { fts_close(walk) }
    // One running total per level, however deep the tree goes.
    var stack = [WorkspaceStorageUsage](repeating: WorkspaceStorageUsage(), count: 64)
    // A file with several names is one file's worth of disk.
    var linked = Set<UInt64>()
    var sinceCheck = 0
    while let entry = fts_read(walk) {
        sinceCheck += 1
        if sinceCheck >= 512 {
            sinceCheck = 0
            if operation.isCancelled {
                return nil
            }
        }
        let level = max(0, Int(entry.pointee.fts_level))
        while level + 1 >= stack.count {
            stack.append(WorkspaceStorageUsage())
        }
        let info = entry.pointee.fts_statp
        let blocksBytes = info.map { UInt64(max(0, $0.pointee.st_blocks)) * 512 } ?? 0
        switch Int32(entry.pointee.fts_info) {
        case FTS_D:
            stack[level] = WorkspaceStorageUsage(bytes: blocksBytes, files: 0)
        case FTS_DP:
            let usage = stack[level]
            var relative = UnsafePointer<CChar>(entry.pointee.fts_path + rootLength)
            while relative.pointee == CChar(UInt8(ascii: "/")) {
                relative += 1
            }
            tree[String(cString: relative)] = usage
            if level > 0 {
                stack[level - 1].bytes += usage.bytes
                stack[level - 1].files += usage.files
            } else {
                total = usage
            }
        case FTS_F, FTS_SL, FTS_SLNONE, FTS_DEFAULT:
            var bytes = blocksBytes
            if let info, info.pointee.st_nlink > 1, !linked.insert(UInt64(info.pointee.st_ino)).inserted {
                bytes = 0
            }
            if level > 0 {
                stack[level - 1].bytes += bytes
                stack[level - 1].files += 1
            } else {
                total = WorkspaceStorageUsage(bytes: bytes, files: 1)
            }
        default:
            // Unreadable, or a directory that couldn't be entered: counted as nothing.
            break
        }
    }
    return (tree, total)
}

/// An entry for what's at `path`, or nil if it's gone. By `lstat`, so a symlink is never a
/// directory to go into.
private func workspaceStorageEntry(name: String, path: String) -> WorkspaceStorageEntry? {
    var info = Darwin.stat()
    guard lstat(path, &info) == 0 else { return nil }
    let entry = WorkspaceStorageEntry(name: name, hostPath: path)
    entry.isDirectory = info.st_mode & S_IFMT == S_IFDIR
    entry.isSymlink = info.st_mode & S_IFMT == S_IFLNK
    entry.bytes = UInt64(max(0, info.st_blocks)) * 512
    entry.files = entry.isDirectory ? 0 : 1
    return entry
}

private func workspaceStorageSorted(_ entries: [WorkspaceStorageEntry]) -> [WorkspaceStorageEntry] {
    entries.sorted { first, second in
        if first.bytes != second.bytes {
            return first.bytes > second.bytes
        }
        return first.name.localizedStandardCompare(second.name) == .orderedAscending
    }
}

/// The rows of one directory of a scanned place, largest first.
private func workspaceStorageChildren(directory: String, relativePath: String, tree: [String: WorkspaceStorageUsage], operation: WorkspaceFileOperation) throws -> [WorkspaceStorageEntry] {
    let names = try FileManager.default.contentsOfDirectory(atPath: directory)
    var children: [WorkspaceStorageEntry] = []
    children.reserveCapacity(names.count)
    for (index, name) in names.enumerated() {
        if index & 0xFF == 0, operation.isCancelled {
            throw WorkspaceFileError.cancelled
        }
        // One deleted between the listing and its lstat isn't a failed listing.
        guard let child = workspaceStorageEntry(name: name, path: WorkspaceFileService.join(directory, name)) else { continue }
        if child.isDirectory {
            let usage = tree[relativePath.isEmpty ? name : relativePath + "/" + name] ?? WorkspaceStorageUsage()
            child.bytes = usage.bytes
            child.files = usage.files
        } else if child.isSymlink {
            child.detail = "Symbolic link"
        }
        children.append(child)
    }
    return workspaceStorageSorted(children)
}

/// The app's container, which is the whole of what this applet looks at. Standardizing can
/// touch the disk, so the first use is off the main thread, by the first scan.
private let workspaceStorageHome = (NSHomeDirectory() as NSString).standardizingPath

/// Every place the app keeps data, with nothing measured yet: each thing in Documents, which
/// is what the shell sees, each thing in Library, and the rest of the container. Nothing in it
/// goes uncounted.
private func workspaceStoragePlaces() -> [WorkspaceStorageEntry] {
    let files = FileManager.default
    let home = workspaceStorageHome
    var places: [WorkspaceStorageEntry] = []

    func add(_ name: String, relativeTo folder: String, title: String, detail: String?) {
        let path = WorkspaceFileService.join(WorkspaceFileService.join(home, folder), name)
        guard let place = workspaceStorageEntry(name: title, path: path) else { return }
        place.detail = detail
        place.placeKey = WorkspaceFileService.join(folder, name)
        place.scanning = true
        place.bytes = 0
        place.files = 0
        places.append(place)
    }

    let documentTitles = ["home": "Home", "tmp": "Temporary files", "fonts": "Fonts"]
    for name in (try? files.contentsOfDirectory(atPath: WorkspaceFileService.join(home, "Documents"))) ?? [] {
        add(name, relativeTo: "Documents", title: documentTitles[name] ?? name,
            detail: WorkspaceFileService.shownPath(WorkspaceFileService.join(WorkspaceFileService.join(home, "Documents"), name)))
    }
    for name in (try? files.contentsOfDirectory(atPath: WorkspaceFileService.join(home, "Library"))) ?? [] {
        add(name, relativeTo: "Library", title: name, detail: "Library")
    }
    for name in (try? files.contentsOfDirectory(atPath: home)) ?? [] where name != "Documents" && name != "Library" {
        add(name, relativeTo: "", title: name == "tmp" ? "App temporary files" : name, detail: nil)
    }
    return places
}

/// What to hand `WorkspaceFileService.remove` for this real path, or nil if Storage won't offer
/// to delete it. It must be inside the app's own folders, and the shell's name for it must lead
/// back to it: a real path the service would read as somewhere else is never offered.
private func workspaceStorageRemovablePath(forHost host: String) -> String? {
    // The same place by either name.
    func comparable(_ path: String) -> String {
        path.hasPrefix("/private/var/") ? String(path.dropFirst("/private".count)) : path
    }
    let target = comparable(host)
    guard target.hasPrefix(comparable(workspaceStorageHome) + "/"), !target.contains("/../") else { return nil }
    let shown = WorkspaceFileService.shownPath(host)
    guard comparable(WorkspaceFileService.hostPath(shown)) == target else { return nil }
    return shown
}

private func workspaceStorageByteString(_ bytes: UInt64) -> String {
    ByteCountFormatter.string(fromByteCount: Int64(clamping: bytes), countStyle: .file)
}

/// Lists what the app's folders hold, largest first, goes into any folder, and deletes what's
/// inside them after asking. Cmd+= and friends scale the rows, the path above them and the
/// summary. The heading and the Refresh button stay, as card furniture does elsewhere.
@MainActor
final class WorkspaceStorageAppletViewController: WorkspaceAppletViewController, WorkspaceTextScalable, WorkspaceStatefulTool, UITableViewDataSource, UITableViewDelegate {
    private static let rowReuseIdentifier = "WorkspaceStorageRow"

    private var summaryLabel: UILabel!
    private var pathLabel: UILabel!
    private let refreshButton = UIButton(type: .system)
    private let tableView = UITableView(frame: .zero, style: .plain)

    private var places: [WorkspaceStorageEntry] = []
    /// While a scan runs. Everything it started stops with it.
    private var scanOperation: WorkspaceFileOperation?
    /// Where the list is: nil at the top, else the place and the directory under it.
    private var currentPlace: WorkspaceStorageEntry?
    private var currentRelativePath = ""
    private var rows: [WorkspaceStorageEntry] = []
    private var listOperation: WorkspaceFileOperation?
    /// The directory `rows` came from, so a new listing of the same one doesn't blank the list.
    private var listedDirectory: String?
    /// Shown where the rows would be when there are none.
    private var placeholder: String?
    /// A place and path to go back to once its scan finishes (a restored window or a refresh).
    private var pendingPlaceKey: String?
    private var pendingRelativePath: String?

    var isScanning: Bool { scanOperation != nil }

    private var rowHeight: CGFloat {
        46 * max(0.8, workspaceTextScale)
    }

    deinit {
        scanOperation?.cancel()
        listOperation?.cancel()
    }

    override func viewDidLoad() {
        super.viewDidLoad()

        let summaryCard = themeCardView()
        toolContentView.addSubview(summaryCard)
        let summaryTitle = themeSecondaryLabel(style: .caption1)
        summaryTitle.text = "APP STORAGE"
        summaryTitle.font = .systemFont(ofSize: 9, weight: .semibold)
        summaryLabel = themeAccentLabel(style: .headline)
        summaryLabel.accessibilityIdentifier = "workspace.storage.summary"
        let summaryText = UIStackView(arrangedSubviews: [summaryTitle, summaryLabel])
        summaryText.axis = .vertical
        summaryText.spacing = 4

        refreshButton.translatesAutoresizingMaskIntoConstraints = false
        refreshButton.layer.cornerRadius = 10
        refreshButton.layer.borderWidth = 1
        refreshButton.contentEdgeInsets = UIEdgeInsets(top: 5, left: 10, bottom: 5, right: 10)
        refreshButton.titleLabel?.font = .systemFont(ofSize: WorkspaceTheme.fontSize(.caption1), weight: .semibold)
        refreshButton.addTarget(self, action: #selector(refreshTapped), for: .touchUpInside)
        refreshButton.setContentHuggingPriority(.required, for: .horizontal)
        refreshButton.setContentCompressionResistancePriority(.required, for: .horizontal)
        refreshButton.accessibilityIdentifier = "workspace.storage.refresh"

        let summaryRow = UIStackView(arrangedSubviews: [summaryText, refreshButton])
        summaryRow.translatesAutoresizingMaskIntoConstraints = false
        summaryRow.axis = .horizontal
        summaryRow.alignment = .center
        summaryRow.spacing = 8
        summaryCard.addSubview(summaryRow)

        let listCard = themeCardView()
        listCard.clipsToBounds = true
        toolContentView.addSubview(listCard)
        pathLabel = themeSecondaryLabel(style: .footnote, monospaced: true)
        pathLabel.lineBreakMode = .byTruncatingHead
        pathLabel.numberOfLines = 1
        pathLabel.accessibilityIdentifier = "workspace.storage.path"
        listCard.addSubview(pathLabel)

        tableView.translatesAutoresizingMaskIntoConstraints = false
        tableView.dataSource = self
        tableView.delegate = self
        tableView.backgroundColor = .clear
        tableView.separatorInset = .zero
        tableView.rowHeight = rowHeight
        tableView.tableFooterView = UIView()
        tableView.accessibilityIdentifier = "workspace.storage.table"
        listCard.addSubview(tableView)

        NSLayoutConstraint.activate([
            summaryCard.topAnchor.constraint(equalTo: toolContentView.topAnchor, constant: 6),
            summaryCard.leadingAnchor.constraint(equalTo: toolContentView.leadingAnchor, constant: 6),
            summaryCard.trailingAnchor.constraint(equalTo: toolContentView.trailingAnchor, constant: -6),
            summaryRow.topAnchor.constraint(equalTo: summaryCard.topAnchor, constant: 10),
            summaryRow.leadingAnchor.constraint(equalTo: summaryCard.leadingAnchor, constant: 12),
            summaryRow.trailingAnchor.constraint(equalTo: summaryCard.trailingAnchor, constant: -10),
            summaryRow.bottomAnchor.constraint(equalTo: summaryCard.bottomAnchor, constant: -10),

            listCard.topAnchor.constraint(equalTo: summaryCard.bottomAnchor, constant: 6),
            listCard.leadingAnchor.constraint(equalTo: toolContentView.leadingAnchor, constant: 6),
            listCard.trailingAnchor.constraint(equalTo: toolContentView.trailingAnchor, constant: -6),
            listCard.bottomAnchor.constraint(equalTo: toolContentView.bottomAnchor, constant: -6),
            pathLabel.topAnchor.constraint(equalTo: listCard.topAnchor, constant: 8),
            pathLabel.leadingAnchor.constraint(equalTo: listCard.leadingAnchor, constant: 12),
            pathLabel.trailingAnchor.constraint(equalTo: listCard.trailingAnchor, constant: -12),
            tableView.topAnchor.constraint(equalTo: pathLabel.bottomAnchor, constant: 6),
            tableView.leadingAnchor.constraint(equalTo: listCard.leadingAnchor),
            tableView.trailingAnchor.constraint(equalTo: listCard.trailingAnchor),
            tableView.bottomAnchor.constraint(equalTo: listCard.bottomAnchor),
        ])

        applyTheme()
        refresh()
    }

    // MARK: - Scanning

    @objc private func refreshTapped() {
        if isScanning {
            cancelScan()
        } else {
            refresh()
        }
    }

    private func refresh() {
        scanOperation?.cancel()
        let operation = WorkspaceFileOperation()
        scanOperation = operation
        // A rescan lands back where the list was, once that place has been read again.
        if let currentPlace {
            pendingPlaceKey = currentPlace.placeKey
            pendingRelativePath = currentRelativePath
        }
        currentPlace = nil
        currentRelativePath = ""
        places = []
        rebuildRows()
        updateSummary()
        updateRefreshButton()

        DispatchQueue.global(qos: .utility).async { [weak self] in
            let found = workspaceStoragePlaces()
            DispatchQueue.main.async { [weak self] in
                MainActor.assumeIsolated {
                    guard let self, self.scanOperation === operation else { return }
                    self.places = found
                    found.forEach { self.scan($0, operation: operation) }
                    self.finishScanIfDone()
                    self.rebuildRows()
                }
            }
        }
    }

    private func scan(_ place: WorkspaceStorageEntry, operation: WorkspaceFileOperation) {
        place.scanning = true
        place.scanSerial += 1
        let serial = place.scanSerial
        let path = place.hostPath
        DispatchQueue.global(qos: .utility).async { [weak self] in
            let result = workspaceStorageScan(path: path, operation: operation)
            DispatchQueue.main.async { [weak self] in
                MainActor.assumeIsolated {
                    guard let self, self.scanOperation === operation, place.scanSerial == serial, let result else { return }
                    place.tree = result.tree
                    place.bytes = result.total.bytes
                    place.files = result.total.files
                    place.scanning = false
                    self.placeDidFinish(place)
                }
            }
        }
    }

    private func cancelScan() {
        scanOperation?.cancel()
        scanOperation = nil
        pendingPlaceKey = nil
        pendingRelativePath = nil
        // A place never measured says so. One being measured again keeps the sizes it had.
        places.forEach { $0.scanning = false }
        if currentPlace == nil {
            rebuildRows()
        }
        summaryLabel.text = "Scan cancelled"
        updateRefreshButton()
    }

    private func placeDidFinish(_ place: WorkspaceStorageEntry) {
        if let pendingPlaceKey, place.placeKey == pendingPlaceKey {
            let relative = pendingRelativePath ?? ""
            self.pendingPlaceKey = nil
            pendingRelativePath = nil
            // Only if it's still there: the directory may be what was deleted.
            if relative.isEmpty || place.tree?[relative] != nil {
                currentPlace = place
                currentRelativePath = relative
            }
        }
        finishScanIfDone()
        if currentPlace == nil || currentPlace === place {
            rebuildRows()
        }
    }

    private func finishScanIfDone() {
        if !places.contains(where: \.scanning) {
            scanOperation = nil
        }
        updateSummary()
        updateRefreshButton()
    }

    private func updateRefreshButton() {
        refreshButton.setTitle(isScanning ? "Cancel" : "Refresh", for: .normal)
    }

    private func updateSummary() {
        guard let summaryLabel else { return }
        if isScanning {
            summaryLabel.text = "Scanning…"
            return
        }
        let used = workspaceStorageByteString(places.reduce(UInt64(0)) { $0 + $1.bytes })
        summaryLabel.text = "\(used) used"
        let documents = WorkspaceFileService.join(workspaceStorageHome, "Documents")
        WorkspaceFileService.shared.filesystemStatus(WorkspaceFileService.shownPath(documents)) { [weak self] result in
            guard let self, !self.isScanning, case .success(let status) = result else { return }
            let free = workspaceStorageByteString(UInt64(max(0, status.available)))
            self.summaryLabel.text = "\(free) free  •  \(used) used"
        }
    }

    // MARK: - The list

    private var currentHostDirectory: String? {
        guard let currentPlace else { return nil }
        return currentRelativePath.isEmpty ? currentPlace.hostPath : WorkspaceFileService.join(currentPlace.hostPath, currentRelativePath)
    }

    private func rebuildRows() {
        listOperation?.cancel()
        listOperation = nil
        guard let place = currentPlace, let directory = currentHostDirectory else {
            listedDirectory = nil
            rows = places.sorted { first, second in
                if first.scanning != second.scanning {
                    return second.scanning
                }
                if first.bytes != second.bytes {
                    return first.bytes > second.bytes
                }
                return first.name.localizedStandardCompare(second.name) == .orderedAscending
            }
            placeholder = rows.isEmpty ? (isScanning ? "Scanning…" : "Nothing found") : nil
            pathLabel?.text = "All storage"
            tableView.reloadData()
            return
        }
        pathLabel?.text = currentRelativePath.isEmpty ? place.name : "\(place.name): /\(currentRelativePath)"
        if listedDirectory != directory {
            listedDirectory = directory
            rows = []
            placeholder = "Loading…"
            tableView.reloadData()
        }

        // A folder can hold many thousands of entries, each with an lstat.
        let operation = WorkspaceFileOperation()
        listOperation = operation
        let relative = currentRelativePath
        let tree = place.tree ?? [:]
        DispatchQueue.global(qos: .userInitiated).async { [weak self] in
            let result = Result { try workspaceStorageChildren(directory: directory, relativePath: relative, tree: tree, operation: operation) }
            DispatchQueue.main.async { [weak self] in
                MainActor.assumeIsolated {
                    guard let self, self.listOperation === operation else { return }
                    self.listOperation = nil
                    switch result {
                    case .success(let children):
                        self.rows = children
                        self.placeholder = children.isEmpty ? "Empty" : nil
                    case .failure(let error):
                        self.rows = []
                        self.placeholder = "Can’t read this folder: \(error.localizedDescription)"
                    }
                    self.tableView.reloadData()
                }
            }
        }
    }

    private var showsBackRow: Bool { currentPlace != nil }

    /// The row an index path shows, if it's one of the entries.
    private func entry(at indexPath: IndexPath) -> WorkspaceStorageEntry? {
        let row = indexPath.row - (showsBackRow ? 1 : 0)
        return rows.indices.contains(row) ? rows[row] : nil
    }

    func tableView(_ tableView: UITableView, numberOfRowsInSection section: Int) -> Int {
        // A directory with nothing in it still says so.
        rows.count + (showsBackRow ? 1 : 0) + (rows.isEmpty && placeholder != nil ? 1 : 0)
    }

    func tableView(_ tableView: UITableView, cellForRowAt indexPath: IndexPath) -> UITableViewCell {
        let cell = tableView.dequeueReusableCell(withIdentifier: Self.rowReuseIdentifier)
            ?? makeCell()
        let colors = theme
        // Set on every dequeue, from the unscaled size: a reused cell carries the font of
        // whatever row it last showed.
        cell.backgroundColor = .clear
        cell.textLabel?.font = .systemFont(ofSize: toolScaledFontSize(WorkspaceTheme.fontSize(.subheadline)), weight: .medium)
        cell.detailTextLabel?.font = .systemFont(ofSize: toolScaledFontSize(WorkspaceTheme.fontSize(.caption1)))
        cell.textLabel?.textColor = colors.primary
        cell.detailTextLabel?.textColor = colors.secondary
        cell.textLabel?.lineBreakMode = .byTruncatingMiddle
        cell.imageView?.tintColor = colors.accent
        cell.accessibilityTraits = .none
        let size = cell.accessoryView as? UILabel
        size?.font = .monospacedDigitSystemFont(ofSize: toolScaledFontSize(WorkspaceTheme.fontSize(.subheadline)), weight: .regular)
        size?.textColor = colors.accent

        if showsBackRow && indexPath.row == 0 {
            cell.textLabel?.text = "‹ Back"
            cell.detailTextLabel?.text = nil
            cell.imageView?.image = nil
            size?.text = nil
            cell.selectionStyle = .default
            cell.accessibilityTraits = .button
            cell.accessibilityIdentifier = "workspace.storage.back"
        } else if let entry = entry(at: indexPath) {
            cell.textLabel?.text = entry.name
            cell.detailTextLabel?.text = detailText(for: entry)
            size?.text = entry.scanning ? "…" : (entry.isUnmeasured ? "—" : workspaceStorageByteString(entry.bytes))
            let symbol = entry.isSymlink ? "link" : (entry.isDirectory ? "folder.fill" : "doc")
            cell.imageView?.image = UIImage(systemName: symbol, withConfiguration: UIImage.SymbolConfiguration(pointSize: 14, weight: .medium))
            cell.selectionStyle = entry.opens ? .default : .none
            cell.accessibilityTraits = entry.opens ? .button : .none
            cell.accessibilityIdentifier = "workspace.storage.row.\(entry.name)"
        } else {
            cell.textLabel?.text = placeholder
            cell.textLabel?.textColor = colors.secondary
            cell.detailTextLabel?.text = nil
            cell.imageView?.image = nil
            size?.text = nil
            cell.selectionStyle = .none
            cell.accessibilityIdentifier = "workspace.storage.placeholder"
        }
        size?.sizeToFit()
        return cell
    }

    private func makeCell() -> UITableViewCell {
        let cell = UITableViewCell(style: .subtitle, reuseIdentifier: Self.rowReuseIdentifier)
        let size = UILabel()
        size.textAlignment = .right
        cell.accessoryView = size
        return cell
    }

    private func detailText(for entry: WorkspaceStorageEntry) -> String? {
        if entry.scanning {
            return "Scanning…"
        }
        if entry.isUnmeasured {
            return "Not measured"
        }
        if let detail = entry.detail {
            return detail
        }
        return entry.isDirectory ? (entry.files == 1 ? "1 item" : "\(entry.files) items") : nil
    }

    func tableView(_ tableView: UITableView, didSelectRowAt indexPath: IndexPath) {
        tableView.deselectRow(at: indexPath, animated: true)
        if showsBackRow && indexPath.row == 0 {
            if currentRelativePath.isEmpty {
                currentPlace = nil
            } else {
                currentRelativePath = (currentRelativePath as NSString).deletingLastPathComponent
            }
            rebuildRows()
            return
        }
        guard let entry = entry(at: indexPath), entry.opens else { return }
        // The person chose where to be, so a scan finishing later doesn't take them elsewhere.
        pendingPlaceKey = nil
        pendingRelativePath = nil
        if currentPlace == nil {
            currentPlace = entry
            currentRelativePath = ""
        } else {
            currentRelativePath = currentRelativePath.isEmpty ? entry.name : currentRelativePath + "/" + entry.name
        }
        rebuildRows()
        if tableView.numberOfRows(inSection: 0) > 0 {
            tableView.scrollToRow(at: IndexPath(row: 0, section: 0), at: .top, animated: false)
        }
    }

    // MARK: - Deleting

    /// A place is never offered, only what's inside one, and only what the service can reach.
    private func removal(at indexPath: IndexPath) -> (entry: WorkspaceStorageEntry, path: String)? {
        guard currentPlace != nil, let entry = entry(at: indexPath),
              let path = workspaceStorageRemovablePath(forHost: entry.hostPath) else { return nil }
        return (entry, path)
    }

    func tableView(_ tableView: UITableView, trailingSwipeActionsConfigurationForRowAt indexPath: IndexPath) -> UISwipeActionsConfiguration? {
        guard let removal = removal(at: indexPath) else { return nil }
        let delete = UIContextualAction(style: .destructive, title: "Delete") { [weak self] _, _, done in
            self?.confirmDelete(removal.entry, path: removal.path)
            // The row stays until it's really gone.
            done(false)
        }
        let configuration = UISwipeActionsConfiguration(actions: [delete])
        configuration.performsFirstActionWithFullSwipe = false
        return configuration
    }

    func tableView(_ tableView: UITableView, contextMenuConfigurationForRowAt indexPath: IndexPath, point: CGPoint) -> UIContextMenuConfiguration? {
        guard let removal = removal(at: indexPath) else { return nil }
        return UIContextMenuConfiguration(identifier: nil, previewProvider: nil) { [weak self] _ in
            UIMenu(children: [
                UIAction(title: "Delete", image: UIImage(systemName: "trash"), attributes: .destructive) { _ in
                    self?.confirmDelete(removal.entry, path: removal.path)
                },
            ])
        }
    }

    private func confirmDelete(_ entry: WorkspaceStorageEntry, path: String) {
        let size = workspaceStorageByteString(entry.bytes)
        let message: String
        if entry.isDirectory {
            let items = entry.files == 1 ? "1 item" : "\(entry.files) items"
            message = "This permanently deletes the folder and the \(items) in it (\(size))."
        } else if entry.isSymlink {
            message = "This removes the link, not what it points to."
        } else {
            message = "This permanently deletes the file (\(size))."
        }
        let alert = UIAlertController(title: "Delete “\(entry.name)”?", message: message, preferredStyle: .alert)
        let delete = UIAlertAction(title: "Delete", style: .destructive) { [weak self] _ in
            self?.delete(entry, path: path)
        }
        delete.accessibilityIdentifier = "workspace.storage.delete.confirm"
        alert.addAction(delete)
        alert.addAction(UIAlertAction(title: "Cancel", style: .cancel))
        (workspaceHost ?? self).present(alert, animated: true)
    }

    private func delete(_ entry: WorkspaceStorageEntry, path: String) {
        let place = currentPlace
        WorkspaceFileService.shared.remove(path, recursive: entry.isDirectory) { [weak self] result in
            guard let self else { return }
            switch result {
            case .success:
                if let place {
                    self.rescan(place)
                }
                if self.listedDirectory != nil {
                    self.rows.removeAll { $0 === entry }
                    self.placeholder = self.rows.isEmpty ? "Empty" : nil
                    self.tableView.reloadData()
                }
            case .failure(let error):
                // The service refuses protected paths, whatever this list offered.
                let alert = UIAlertController(title: "Can’t Delete", message: error.localizedDescription, preferredStyle: .alert)
                alert.addAction(UIAlertAction(title: "OK", style: .default))
                (self.workspaceHost ?? self).present(alert, animated: true)
            }
        }
    }

    /// The rest of the list isn't worth reading again after one thing went from one place.
    private func rescan(_ place: WorkspaceStorageEntry) {
        let operation = scanOperation ?? WorkspaceFileOperation()
        scanOperation = operation
        scan(place, operation: operation)
        updateSummary()
        updateRefreshButton()
    }

    // MARK: - Workspace

    /// Which directory the list was in: a restored window back at the top has forgotten what
    /// was being looked at.
    func workspaceToolStateForSaving() -> [String: Any]? {
        guard let place = currentPlace?.placeKey ?? pendingPlaceKey else { return nil }
        return ["place": place, "path": (currentPlace != nil ? currentRelativePath : pendingRelativePath) ?? ""]
    }

    func workspaceRestoreToolState(_ state: [String: Any]) {
        guard let place = state["place"] as? String, !place.isEmpty else { return }
        // Applied when that place's scan finishes; until then the list is the top.
        pendingPlaceKey = place
        pendingRelativePath = state["path"] as? String ?? ""
        if let finished = places.first(where: { !$0.scanning && $0.tree != nil && $0.placeKey == place }) {
            placeDidFinish(finished)
        }
    }

    // MARK: - Text scale and theme

    override func applyTextScale() {
        super.applyTextScale()
        guard let summaryLabel, let pathLabel else { return }
        scaleLabel(summaryLabel)
        scaleLabel(pathLabel)
        tableView.rowHeight = rowHeight
        tableView.reloadData()
    }

    override func applyTheme() {
        super.applyTheme()
        guard let summaryLabel else { return }
        let colors = theme
        summaryLabel.textColor = colors.accent
        refreshButton.backgroundColor = colors.cardAlt.withAlphaComponent(0.96)
        refreshButton.layer.borderColor = colors.stroke.cgColor
        refreshButton.setTitleColor(colors.accentAlt, for: .normal)
        tableView.separatorColor = colors.stroke
        tableView.reloadData()
    }
}
