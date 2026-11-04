// Copyright 2026 Mike Miller
// SPDX-License-Identifier: Apache-2.0
//
// Ported by its author from iSH-AOK's Workspace (app/WorkspaceViewController.m at
// 0f75fee1c) and licensed to PSCAL under the Apache License 2.0.

import UIKit

extension Notification.Name {
    static let workspaceLauncherShortcutsDidChange = Notification.Name("WorkspaceLauncherShortcutsDidChange")
}

/// The user's shortcuts, one tree in UserDefaults. A shortcut is `{name, command}`, a leaf
/// that runs a command in a fresh terminal, or `{name, children}`, a group holding more of
/// them, so "Remote Login" can hold several hosts. A path is a chain of child indices from
/// the root; an empty one is the root.
@MainActor
enum WorkspaceLauncherStore {
    typealias Shortcut = [String: Any]

    private static let key = "com.pscal.workspace.launcherShortcuts"

    static var shortcuts: [Shortcut] {
        get { UserDefaults.standard.array(forKey: key) as? [Shortcut] ?? [] }
        set {
            UserDefaults.standard.set(newValue, forKey: key)
            NotificationCenter.default.post(name: .workspaceLauncherShortcutsDidChange, object: nil)
        }
    }

    static func isGroup(_ shortcut: Shortcut?) -> Bool {
        shortcut?["children"] is [Any]
    }

    static func children(of shortcut: Shortcut) -> [Shortcut] {
        shortcut["children"] as? [Shortcut] ?? []
    }

    static func displayName(of shortcut: Shortcut) -> String {
        if let name = shortcut["name"] as? String, !name.isEmpty { return name }
        if let command = shortcut["command"] as? String, !command.isEmpty { return command }
        return "Terminal"
    }

    /// The level at `path`, or nil for a path that no longer resolves.
    private static func level(in root: [Shortcut], at path: [Int]) -> [Shortcut]? {
        var current = root
        for index in path {
            guard current.indices.contains(index) else { return nil }
            current = children(of: current[index])
        }
        return current
    }

    static func array(atPath path: [Int]) -> [Shortcut] {
        level(in: shortcuts, at: path) ?? []
    }

    private static func update(_ current: inout [Shortcut], path: ArraySlice<Int>, _ body: (inout [Shortcut]) -> Void) {
        guard let index = path.first else {
            body(&current)
            return
        }
        guard current.indices.contains(index) else { return }
        var inner = children(of: current[index])
        update(&inner, path: path.dropFirst(), body)
        current[index]["children"] = inner
    }

    /// Changes the level at `path` and saves the tree. A stale path does nothing.
    static func mutate(atPath path: [Int], _ body: (inout [Shortcut]) -> Void) {
        var root = shortcuts
        guard level(in: root, at: path) != nil else { return }
        update(&root, path: path[...], body)
        shortcuts = root
    }

    private static let movedMarker = "__moved"

    private static func removeMoved(_ level: inout [Shortcut]) {
        level.removeAll { $0[movedMarker] != nil }
        for index in level.indices where isGroup(level[index]) {
            var inner = children(of: level[index])
            removeMoved(&inner)
            level[index]["children"] = inner
        }
    }

    /// Moves a shortcut between two levels in one write, so the tree never loses it. Both
    /// paths describe the tree as it is now, and one can run through the array being taken
    /// from, so a marker holds the place until the shortcut has landed: removing it first
    /// would shift the indices and could send it to the wrong group, or nowhere.
    static func move(fromPath: [Int], sourceIndex: Int, toPath: [Int], destinationIndex: Int) {
        var root = shortcuts
        guard let source = level(in: root, at: fromPath), source.indices.contains(sourceIndex),
              level(in: root, at: toPath) != nil else { return }
        let moved = source[sourceIndex]
        update(&root, path: fromPath[...]) { $0[sourceIndex] = [movedMarker: true] }
        update(&root, path: toPath[...]) { $0.insert(moved, at: min(destinationIndex, $0.count)) }
        removeMoved(&root)
        shortcuts = root
    }

    private static func token(in command: String) -> String? {
        let trimmed = command.trimmingCharacters(in: .whitespacesAndNewlines)
        guard trimmed.count >= 3, trimmed.hasPrefix("{"), trimmed.hasSuffix("}") else { return nil }
        return trimmed.dropFirst().dropLast().trimmingCharacters(in: .whitespaces).lowercased()
    }

    /// A command that's just a {token} opens a built-in instead of running in a shell.
    static func toolIdentifier(forCommand command: String) -> String? {
        guard let token = token(in: command) else { return nil }
        let map: [String: String] = [
            "browser": WorkspaceToolRegistry.browser,
            "web": WorkspaceToolRegistry.browser,
            "web_browser": WorkspaceToolRegistry.browser,
            "webbrowser": WorkspaceToolRegistry.browser,
            "clock": WorkspaceToolRegistry.clock,
            "settings": WorkspaceToolRegistry.settings,
            "themes": WorkspaceToolRegistry.themes,
            "sessions": WorkspaceToolRegistry.sessions,
            "storage": WorkspaceToolRegistry.storage,
            "monitor": WorkspaceToolRegistry.monitor,
            "networks": WorkspaceToolRegistry.networks,
            "logs": WorkspaceToolRegistry.logs,
            "status": WorkspaceToolRegistry.logs,
            "diagnostics": WorkspaceToolRegistry.diagnostics,
            "workspaces": WorkspaceToolRegistry.desktops,
            "shortcuts": WorkspaceToolRegistry.quickActions,
            "quick_actions": WorkspaceToolRegistry.quickActions,
            "music": WorkspaceToolRegistry.music,
            "audio": WorkspaceToolRegistry.music,
            "equalizer": WorkspaceToolRegistry.equalizer,
            "eq": WorkspaceToolRegistry.equalizer,
            "motepad": WorkspaceToolRegistry.motePad,
            "editor": WorkspaceToolRegistry.motePad,
            "notepad": WorkspaceToolRegistry.motePad,
            "textedit": WorkspaceToolRegistry.motePad,
            "text": WorkspaceToolRegistry.motePad,
            "files": WorkspaceToolRegistry.fileManager,
            "filemanager": WorkspaceToolRegistry.fileManager,
            "finder": WorkspaceToolRegistry.fileManager,
            "markdown": WorkspaceToolRegistry.markdown,
            "md": WorkspaceToolRegistry.markdown,
            "imageviewer": WorkspaceToolRegistry.imageViewer,
            "image": WorkspaceToolRegistry.imageViewer,
            "img": WorkspaceToolRegistry.imageViewer,
            "videoplayer": WorkspaceToolRegistry.videoPlayer,
            "video": WorkspaceToolRegistry.videoPlayer,
        ]
        return map[token]
    }

    /// {shell} asks for a terminal rather than an applet.
    static func isTerminalToken(_ command: String) -> Bool {
        guard let token = token(in: command) else { return false }
        return ["shell", "session_shell", "sessionshell"].contains(token)
    }

    /// What Add Built-in offers.
    static var builtIns: [(name: String, command: String)] {
        let all: [(name: String, command: String, identifier: String?)] = [
            ("New Terminal", "", nil),
            ("Web Browser", "{browser}", WorkspaceToolRegistry.browser),
            ("Music", "{music}", WorkspaceToolRegistry.music),
            ("Equalizer", "{equalizer}", WorkspaceToolRegistry.equalizer),
            ("MotePad", "{motepad}", WorkspaceToolRegistry.motePad),
            ("File Manager", "{files}", WorkspaceToolRegistry.fileManager),
            ("Markdown", "{markdown}", WorkspaceToolRegistry.markdown),
            ("Image Viewer", "{imageviewer}", WorkspaceToolRegistry.imageViewer),
            ("Video Player", "{videoplayer}", WorkspaceToolRegistry.videoPlayer),
            ("Clock", "{clock}", WorkspaceToolRegistry.clock),
            ("Monitor", "{monitor}", WorkspaceToolRegistry.monitor),
            ("Networks", "{networks}", WorkspaceToolRegistry.networks),
            ("Logs", "{logs}", WorkspaceToolRegistry.logs),
            ("Storage", "{storage}", WorkspaceToolRegistry.storage),
            ("Themes", "{themes}", WorkspaceToolRegistry.themes),
            ("Sessions", "{sessions}", WorkspaceToolRegistry.sessions),
            ("Diagnostics", "{diagnostics}", WorkspaceToolRegistry.diagnostics),
            ("Settings", "{settings}", WorkspaceToolRegistry.settings),
        ]
        return all.filter { entry in
            entry.identifier.map(WorkspaceToolRegistry.isAvailable) ?? true
        }.map { ($0.name, $0.command) }
    }
}

/// The Launcher's sizes in one place, so the window and the list can't disagree.
@MainActor
enum WorkspaceLauncherMetrics {
    static func rowHeight(textScale: CGFloat) -> CGFloat {
        ((WorkspaceMetrics.usesPhoneLayout ? 34 : 38) * textScale).rounded(.up)
    }

    static func backRowHeight(textScale: CGFloat) -> CGFloat {
        ((WorkspaceMetrics.usesPhoneLayout ? 28 : 30) * textScale).rounded(.up)
    }

    /// The Add and Edit footer doesn't scale, and the width only grows: narrower than the
    /// default, those fixed-size pills would clip.
    static func contentSize(itemCount: Int, showsBackRow: Bool, textScale: CGFloat) -> CGSize {
        let phone = WorkspaceMetrics.usesPhoneLayout
        let inset: CGFloat = phone ? 12 : 16
        let spacing: CGFloat = 8
        let defaultWidth: CGFloat = phone ? 168 : 200
        let width = max(defaultWidth, (defaultWidth * textScale).rounded(.up))
        let footer: CGFloat = (phone ? 26 : 28) + 8
        let items = itemCount > 0
            ? CGFloat(itemCount) * rowHeight(textScale: textScale)
            : ((phone ? 28 : 32) * textScale).rounded(.up)
        var height = inset + items + spacing + footer + inset + WorkspaceMetrics.titleBarHeight
        if showsBackRow {
            height += backRowHeight(textScale: textScale) + spacing
        }
        return CGSize(width: width, height: height)
    }
}

/// The user's shortcuts as a list on the desktop. Tap to run one or go into a group; Edit
/// deletes and drags them, including into a group and out through the Back row.
///
/// Reordering is the table's own drag and drop. The classic reorder API can only move within
/// one flat array, and these moves go between levels of a tree.
@MainActor
final class WorkspaceLauncherAppletViewController: WorkspaceAppletViewController, WorkspaceTextScalable, WorkspaceStatefulTool,
    UITableViewDataSource, UITableViewDelegate, UITableViewDragDelegate, UITableViewDropDelegate, UIDropInteractionDelegate {
    private static let rowReuseIdentifier = "launcher.row"

    private var contentStack: UIStackView!
    private let tableView = UITableView(frame: .zero, style: .plain)
    private var tableHeight: NSLayoutConstraint!
    private var currentPath: [Int] = []
    private var isEditingList = false
    private var rowShortcuts: [WorkspaceLauncherStore.Shortcut] = []
    /// Set before an edit made here, so the notification it posts doesn't undo the table's
    /// own animation with a reload. Other Launcher lists still react.
    private var suppressNextReload = false
    private var observer: NSObjectProtocol?

    deinit {
        if let observer {
            NotificationCenter.default.removeObserver(observer)
        }
    }

    override func viewDidLoad() {
        super.viewDidLoad()
        let inset: CGFloat = WorkspaceMetrics.usesPhoneLayout ? 12 : 16
        let scrolling = installScrollingStack(spacing: 8, inset: inset)
        scrolling.scroll.alwaysBounceVertical = false
        contentStack = scrolling.stack

        tableView.translatesAutoresizingMaskIntoConstraints = false
        tableView.dataSource = self
        tableView.delegate = self
        tableView.dragDelegate = self
        tableView.dropDelegate = self
        tableView.dragInteractionEnabled = true
        tableView.allowsSelectionDuringEditing = true
        tableView.backgroundColor = .clear
        tableView.separatorInset = .zero
        // The outer scroll view is the one that scrolls, and this is sized to its rows, so
        // no pan competes with a drag inside the list.
        tableView.isScrollEnabled = false
        tableView.rowHeight = WorkspaceLauncherMetrics.rowHeight(textScale: workspaceTextScale)
        tableView.separatorColor = theme.stroke
        tableView.register(UITableViewCell.self, forCellReuseIdentifier: Self.rowReuseIdentifier)
        tableHeight = tableView.heightAnchor.constraint(equalToConstant: tableView.rowHeight)
        tableHeight.isActive = true

        observer = NotificationCenter.default.addObserver(forName: .workspaceLauncherShortcutsDidChange, object: nil, queue: .main) { [weak self] _ in
            MainActor.assumeIsolated {
                self?.shortcutsDidChange()
            }
        }
        rebuildList()
    }

    override func viewDidAppear(_ animated: Bool) {
        super.viewDidAppear(animated)
        autosizeWindow()
    }

    override func themeDidChange() {
        super.themeDidChange()
        rebuildList()
    }

    private func shortcutsDidChange() {
        if suppressNextReload {
            suppressNextReload = false
            return
        }
        clampPath()
        rebuildList()
    }

    /// An edit elsewhere can delete the group this list is inside.
    private func clampPath() {
        while let last = currentPath.last {
            let parent = WorkspaceLauncherStore.array(atPath: Array(currentPath.dropLast()))
            if parent.indices.contains(last), WorkspaceLauncherStore.isGroup(parent[last]) {
                return
            }
            currentPath.removeLast()
        }
    }

    // MARK: - Text scale and state

    override func applyTextScale() {
        super.applyTextScale()
        guard contentStack != nil else { return }
        tableView.rowHeight = WorkspaceLauncherMetrics.rowHeight(textScale: workspaceTextScale)
        rebuildList()
    }

    /// Which group the list was in. Cheap to carry, and a Launcher that jumps back to the
    /// top looks like it forgot where you were.
    func workspaceToolStateForSaving() -> [String: Any]? {
        currentPath.isEmpty ? nil : ["path": currentPath]
    }

    func workspaceRestoreToolState(_ state: [String: Any]) {
        guard let path = state["path"] as? [Int] else { return }
        currentPath = path
        clampPath()
        rebuildList()
    }

    // MARK: - The list

    private func autosizeWindow() {
        let size = WorkspaceLauncherMetrics.contentSize(itemCount: rowShortcuts.count,
                                                        showsBackRow: !currentPath.isEmpty,
                                                        textScale: workspaceTextScale)
        guard let host = workspaceHost, let window = host.window(holding: self) else { return }
        host.resizeWindow(window, to: size)
    }

    private func rebuildList() {
        guard let contentStack else { return }
        clearArrangedSubviews(of: contentStack)
        if !currentPath.isEmpty {
            contentStack.addArrangedSubview(makeBackButton())
        }
        rowShortcuts = WorkspaceLauncherStore.array(atPath: currentPath)
        tableView.separatorColor = theme.stroke
        if rowShortcuts.isEmpty {
            let empty = themeSecondaryLabel(style: .footnote)
            empty.font = .systemFont(ofSize: toolScaledFontSize(WorkspaceTheme.fontSize(.footnote)))
            empty.text = currentPath.isEmpty ? "No shortcuts yet." : "No shortcuts in this group yet."
            empty.textColor = theme.secondary
            contentStack.addArrangedSubview(empty)
        } else {
            contentStack.addArrangedSubview(tableView)
        }
        updateTableHeight()
        tableView.setEditing(isEditingList, animated: false)
        tableView.reloadData()
        contentStack.addArrangedSubview(makeBottomControls())
        autosizeWindow()
    }

    private func updateTableHeight() {
        tableHeight.constant = tableView.rowHeight * CGFloat(max(rowShortcuts.count, 1))
    }

    func tableView(_ tableView: UITableView, numberOfRowsInSection section: Int) -> Int {
        rowShortcuts.count
    }

    func tableView(_ tableView: UITableView, cellForRowAt indexPath: IndexPath) -> UITableViewCell {
        let cell = tableView.dequeueReusableCell(withIdentifier: Self.rowReuseIdentifier, for: indexPath)
        let shortcut = rowShortcuts.indices.contains(indexPath.row) ? rowShortcuts[indexPath.row] : nil
        let group = WorkspaceLauncherStore.isGroup(shortcut)
        cell.backgroundColor = .clear
        cell.textLabel?.text = shortcut.map(WorkspaceLauncherStore.displayName(of:)) ?? "Terminal"
        // On every dequeue, from the unscaled size: a reused cell carries the font of
        // whatever row it last showed.
        cell.textLabel?.font = .systemFont(ofSize: toolScaledFontSize(WorkspaceTheme.fontSize(.subheadline)), weight: .medium)
        cell.textLabel?.textColor = theme.primary
        cell.accessoryType = group && !isEditingList ? .disclosureIndicator : .none
        // A folder marks a group in edit mode too, where the chevron gives way to the drag
        // hint, and that's when it matters which rows take a drop.
        cell.imageView?.image = group
            ? UIImage(systemName: "folder.fill", withConfiguration: UIImage.SymbolConfiguration(pointSize: 15, weight: .medium))
            : nil
        cell.imageView?.tintColor = theme.accent
        cell.editingAccessoryView = isEditingList ? makeReorderHint() : nil
        return cell
    }

    /// Drag and drop has no handle of its own, so edit mode needs a hint that rows move.
    private func makeReorderHint() -> UIView {
        let icon = UIImageView(image: UIImage(systemName: "line.3.horizontal",
                                              withConfiguration: UIImage.SymbolConfiguration(pointSize: 15, weight: .semibold)))
        icon.tintColor = theme.secondary
        icon.contentMode = .center
        icon.frame = CGRect(x: 0, y: 0, width: 28, height: 28)
        return icon
    }

    /// A tap always goes into a group, in edit mode too: that's the only way to reach what's
    /// inside. A leaf edits while editing and runs otherwise.
    func tableView(_ tableView: UITableView, didSelectRowAt indexPath: IndexPath) {
        tableView.deselectRow(at: indexPath, animated: true)
        guard rowShortcuts.indices.contains(indexPath.row) else { return }
        let shortcut = rowShortcuts[indexPath.row]
        if WorkspaceLauncherStore.isGroup(shortcut) {
            currentPath.append(indexPath.row)
            rebuildList()
            return
        }
        if isEditingList {
            workspaceHost?.presentEditLauncherShortcut(path: currentPath, index: indexPath.row)
            return
        }
        workspaceHost?.runLauncherShortcut(command: shortcut["command"] as? String ?? "", title: shortcut["name"] as? String)
    }

    /// While editing, a long press gives the rename and delete dialog. UIKit tells holding
    /// still for a menu from holding to drag, which a second gesture recognizer wouldn't.
    func tableView(_ tableView: UITableView, contextMenuConfigurationForRowAt indexPath: IndexPath, point: CGPoint) -> UIContextMenuConfiguration? {
        guard isEditingList, rowShortcuts.indices.contains(indexPath.row) else { return nil }
        let index = indexPath.row
        let group = WorkspaceLauncherStore.isGroup(rowShortcuts[index])
        let path = currentPath
        return UIContextMenuConfiguration(identifier: nil, previewProvider: nil) { [weak self] _ in
            let edit = UIAction(title: group ? "Edit Group…" : "Edit Shortcut…", image: UIImage(systemName: "pencil")) { _ in
                self?.workspaceHost?.presentEditLauncherShortcut(path: path, index: index)
            }
            guard group else { return UIMenu(title: "", children: [edit]) }
            // A group can be added to without going into it first.
            let add = UIAction(title: "Add…", image: UIImage(systemName: "plus")) { _ in
                guard let self else { return }
                self.workspaceHost?.presentAddLauncherOptions(from: self.view, path: path + [index])
            }
            return UIMenu(title: "", children: [add, edit])
        }
    }

    func tableView(_ tableView: UITableView, canEditRowAt indexPath: IndexPath) -> Bool {
        true
    }

    func tableView(_ tableView: UITableView, editingStyleForRowAt indexPath: IndexPath) -> UITableViewCell.EditingStyle {
        .delete
    }

    func tableView(_ tableView: UITableView, commit editingStyle: UITableViewCell.EditingStyle, forRowAt indexPath: IndexPath) {
        guard editingStyle == .delete, rowShortcuts.indices.contains(indexPath.row) else { return }
        let shortcut = rowShortcuts[indexPath.row]
        let count = WorkspaceLauncherStore.children(of: shortcut).count
        guard count > 0 else {
            deleteRow(at: indexPath)
            return
        }
        let name = WorkspaceLauncherStore.displayName(of: shortcut)
        let message = count == 1
            ? "This deletes \"\(name)\" and 1 item inside it."
            : "This deletes \"\(name)\" and \(count) items inside it."
        let confirm = UIAlertController(title: "Delete Group?", message: message, preferredStyle: .alert)
        confirm.addAction(UIAlertAction(title: "Delete", style: .destructive) { [weak self] _ in
            self?.deleteRow(at: indexPath)
        })
        confirm.addAction(UIAlertAction(title: "Cancel", style: .cancel))
        (workspaceHost ?? self).present(confirm, animated: true)
    }

    private func deleteRow(at indexPath: IndexPath) {
        let index = indexPath.row
        guard rowShortcuts.indices.contains(index) else { return }
        rowShortcuts.remove(at: index)
        suppressNextReload = true
        WorkspaceLauncherStore.mutate(atPath: currentPath) { level in
            if level.indices.contains(index) {
                level.remove(at: index)
            }
        }
        if rowShortcuts.isEmpty {
            // Swap the table for the empty text rather than animating down to no rows.
            rebuildList()
            return
        }
        tableView.deleteRows(at: [indexPath], with: .fade)
        updateTableHeight()
        UIView.animate(withDuration: 0.25) {
            self.view.layoutIfNeeded()
        }
        autosizeWindow()
    }

    // MARK: - Drag and drop

    /// A move inside the app: the provider's content is never read, and the payload is the
    /// row, resolved at the drop.
    func tableView(_ tableView: UITableView, itemsForBeginning session: UIDragSession, at indexPath: IndexPath) -> [UIDragItem] {
        guard isEditingList, rowShortcuts.indices.contains(indexPath.row) else { return [] }
        let item = UIDragItem(itemProvider: NSItemProvider())
        item.localObject = indexPath.row
        return [item]
    }

    func tableView(_ tableView: UITableView, canHandle session: UIDropSession) -> Bool {
        isEditingList && session.localDragSession != nil
    }

    /// Onto a group nests inside it; between rows reorders, as Files does with folders.
    func tableView(_ tableView: UITableView, dropSessionDidUpdate session: UIDropSession, withDestinationIndexPath destinationIndexPath: IndexPath?) -> UITableViewDropProposal {
        guard let destinationIndexPath, rowShortcuts.indices.contains(destinationIndexPath.row) else {
            return UITableViewDropProposal(operation: .move, intent: .insertAtDestinationIndexPath)
        }
        let group = WorkspaceLauncherStore.isGroup(rowShortcuts[destinationIndexPath.row])
        return UITableViewDropProposal(operation: .move, intent: group ? .insertIntoDestinationIndexPath : .insertAtDestinationIndexPath)
    }

    func tableView(_ tableView: UITableView, performDropWith coordinator: UITableViewDropCoordinator) {
        guard let item = coordinator.items.first, let sourcePath = item.sourceIndexPath,
              let from = item.dragItem.localObject as? Int, rowShortcuts.indices.contains(from) else { return }
        let destination = coordinator.destinationIndexPath
        let to = destination?.row ?? rowShortcuts.count - 1
        let intoGroup = destination != nil && to != from && rowShortcuts.indices.contains(to)
            && WorkspaceLauncherStore.isGroup(rowShortcuts[to])
        if intoGroup {
            suppressNextReload = true
            WorkspaceLauncherStore.move(fromPath: currentPath, sourceIndex: from,
                                        toPath: currentPath + [to], destinationIndex: Int.max)
            rowShortcuts.remove(at: from)
            rebuildList()
        } else if to != from, let destination {
            let moved = rowShortcuts.remove(at: from)
            rowShortcuts.insert(moved, at: to)
            let order = rowShortcuts
            suppressNextReload = true
            WorkspaceLauncherStore.mutate(atPath: currentPath) { $0 = order }
            tableView.moveRow(at: sourcePath, to: destination)
        }
        coordinator.drop(item.dragItem, toRowAt: destination ?? sourcePath)
    }

    // MARK: - The Back row as a drop target

    func dropInteraction(_ interaction: UIDropInteraction, canHandle session: UIDropSession) -> Bool {
        isEditingList && !currentPath.isEmpty && session.localDragSession != nil
    }

    func dropInteraction(_ interaction: UIDropInteraction, sessionDidUpdate session: UIDropSession) -> UIDropProposal {
        UIDropProposal(operation: .move)
    }

    func dropInteraction(_ interaction: UIDropInteraction, sessionDidEnter session: UIDropSession) {
        interaction.view?.backgroundColor = theme.accent.withAlphaComponent(0.2)
    }

    func dropInteraction(_ interaction: UIDropInteraction, sessionDidExit session: UIDropSession) {
        interaction.view?.backgroundColor = .clear
    }

    /// Lands right after the group it came out of, so it stays close to where it was.
    func dropInteraction(_ interaction: UIDropInteraction, performDrop session: UIDropSession) {
        interaction.view?.backgroundColor = .clear
        guard let group = currentPath.last,
              let source = session.items.first?.localObject as? Int,
              rowShortcuts.indices.contains(source) else { return }
        suppressNextReload = true
        WorkspaceLauncherStore.move(fromPath: currentPath, sourceIndex: source,
                                    toPath: Array(currentPath.dropLast()), destinationIndex: group + 1)
        rowShortcuts.remove(at: source)
        rebuildList()
    }

    // MARK: - Controls

    private func makeBackButton() -> UIButton {
        let back = UIButton(type: .system)
        back.translatesAutoresizingMaskIntoConstraints = false
        back.setTitle("‹ Back", for: .normal)
        back.setTitleColor(theme.primary, for: .normal)
        back.titleLabel?.font = .systemFont(ofSize: toolScaledFontSize(WorkspaceTheme.fontSize(.subheadline)), weight: .semibold)
        back.contentHorizontalAlignment = .center
        back.heightAnchor.constraint(greaterThanOrEqualToConstant: WorkspaceLauncherMetrics.backRowHeight(textScale: workspaceTextScale)).isActive = true
        back.addTarget(self, action: #selector(backTapped), for: .touchUpInside)
        if isEditingList {
            // Dragging an item here is the only way to un-nest it: the list shows one level
            // at a time, so the parent isn't otherwise on screen.
            back.layer.borderWidth = 1
            back.layer.cornerRadius = 12
            back.layer.borderColor = theme.stroke.cgColor
            back.addInteraction(UIDropInteraction(delegate: self))
        }
        return back
    }

    /// Unbordered and muted beside the rows, so they read as controls and not as shortcuts.
    private func makePill(title: String, identifier: String, action: Selector) -> UIButton {
        let pill = UIButton(type: .system)
        pill.translatesAutoresizingMaskIntoConstraints = false
        pill.setTitle(title, for: .normal)
        pill.setTitleColor(theme.secondary, for: .normal)
        pill.titleLabel?.font = .systemFont(ofSize: WorkspaceTheme.fontSize(.footnote), weight: .medium)
        pill.backgroundColor = theme.cardAlt.withAlphaComponent(0.25)
        pill.layer.cornerRadius = 10
        pill.accessibilityIdentifier = identifier
        pill.heightAnchor.constraint(equalToConstant: WorkspaceMetrics.usesPhoneLayout ? 26 : 28).isActive = true
        pill.addTarget(self, action: action, for: .touchUpInside)
        return pill
    }

    private func makeBottomControls() -> UIView {
        let row = UIStackView()
        row.translatesAutoresizingMaskIntoConstraints = false
        row.axis = .horizontal
        row.spacing = 8
        row.isLayoutMarginsRelativeArrangement = true
        row.layoutMargins = UIEdgeInsets(top: 4, left: 8, bottom: 4, right: 8)
        row.distribution = .fillEqually
        row.addArrangedSubview(makePill(title: "+ Add", identifier: "workspace.launcher.add", action: #selector(addTapped(_:))))
        row.addArrangedSubview(makePill(title: isEditingList ? "Done" : "Edit", identifier: "workspace.launcher.edit", action: #selector(editTapped)))
        return row
    }

    @objc private func addTapped(_ sender: UIButton) {
        workspaceHost?.presentAddLauncherOptions(from: sender, path: currentPath)
    }

    @objc private func editTapped() {
        isEditingList.toggle()
        rebuildList()
    }

    @objc private func backTapped() {
        guard !currentPath.isEmpty else { return }
        currentPath.removeLast()
        rebuildList()
    }
}

// MARK: - Running and editing shortcuts

extension WorkspaceViewController {
    /// A {token} opens the matching built-in; anything else runs in a fresh terminal, and an
    /// empty command just opens one.
    func runLauncherShortcut(command: String, title: String?) {
        if WorkspaceLauncherStore.isTerminalToken(command) {
            openNewTerminalWindow()
            return
        }
        if let identifier = WorkspaceLauncherStore.toolIdentifier(forCommand: command) {
            openOrFocusTool(identifier)
            return
        }
        let name = (title?.isEmpty == false ? title : nil) ?? (command.isEmpty ? nil : command)
        openNewTerminalWindow(runningOnce: command, title: name)
    }

    private func presentLauncherSheet(title: String, message: String?, actions: [UIAlertAction], from sourceView: UIView, sourceRect: CGRect) {
        let sheet = UIAlertController(title: title, message: message, preferredStyle: .actionSheet)
        actions.forEach(sheet.addAction)
        sheet.addAction(UIAlertAction(title: "Cancel", style: .cancel))
        // An iPad presents this as a popover, which must have an anchor or UIKit raises.
        sheet.popoverPresentationController?.sourceView = sourceView
        sheet.popoverPresentationController?.sourceRect = sourceRect
        present(sheet, animated: true)
    }

    /// Presenting from an alert action's handler races the alert's dismissal, and UIKit drops
    /// the new one.
    private func nextTurn(_ work: @escaping @MainActor () -> Void) {
        DispatchQueue.main.async {
            MainActor.assumeIsolated(work)
        }
    }

    /// The quick-launch popup. A group shows this same sheet for its children, with a Back
    /// action, so nested shortcuts can be reached without leaving it.
    func presentLauncher(from sourceView: UIView, sourceRect: CGRect, path: [Int] = []) {
        let shortcuts = WorkspaceLauncherStore.array(atPath: path)
        var actions: [UIAlertAction] = []
        for (index, shortcut) in shortcuts.enumerated() {
            let name = WorkspaceLauncherStore.displayName(of: shortcut)
            if WorkspaceLauncherStore.isGroup(shortcut) {
                actions.append(UIAlertAction(title: name + " ›", style: .default) { [weak self] _ in
                    self?.nextTurn {
                        self?.presentLauncher(from: sourceView, sourceRect: sourceRect, path: path + [index])
                    }
                })
            } else {
                let command = shortcut["command"] as? String ?? ""
                actions.append(UIAlertAction(title: name, style: .default) { [weak self] _ in
                    self?.runLauncherShortcut(command: command, title: name)
                })
            }
        }
        if !path.isEmpty {
            actions.append(UIAlertAction(title: "‹ Back", style: .default) { [weak self] _ in
                self?.nextTurn {
                    self?.presentLauncher(from: sourceView, sourceRect: sourceRect, path: Array(path.dropLast()))
                }
            })
        } else {
            actions.append(UIAlertAction(title: "Show on Desktop", style: .default) { [weak self] _ in
                self?.openOrFocusTool(WorkspaceToolRegistry.launcher)
            })
        }
        actions.append(UIAlertAction(title: "Edit Shortcuts…", style: .default) { [weak self] _ in
            self?.nextTurn {
                self?.presentLauncherEditor(from: sourceView, path: path)
            }
        })
        presentLauncherSheet(title: "Launcher",
                             message: shortcuts.isEmpty ? "Add a shortcut to run a command in a new terminal." : nil,
                             actions: actions, from: sourceView, sourceRect: sourceRect)
    }

    private func addActions(from sourceView: UIView, path: [Int]) -> [UIAlertAction] {
        [
            UIAlertAction(title: "Add Shortcut…", style: .default) { [weak self] _ in
                self?.nextTurn { self?.presentAddLauncherShortcut(path: path) }
            },
            UIAlertAction(title: "Add Built-in…", style: .default) { [weak self] _ in
                self?.nextTurn { self?.presentAddLauncherBuiltIn(from: sourceView, path: path) }
            },
            UIAlertAction(title: "Add Group…", style: .default) { [weak self] _ in
                self?.nextTurn { self?.presentAddLauncherGroup(path: path) }
            },
        ]
    }

    /// Just the three add actions, for the Launcher's own Add button.
    func presentAddLauncherOptions(from sourceView: UIView, path: [Int]) {
        presentLauncherSheet(title: "Add", message: nil, actions: addActions(from: sourceView, path: path),
                             from: sourceView, sourceRect: sourceView.bounds)
    }

    private func presentLauncherEditor(from sourceView: UIView, path: [Int]) {
        let shortcuts = WorkspaceLauncherStore.array(atPath: path)
        var actions: [UIAlertAction] = []
        for (index, shortcut) in shortcuts.enumerated() {
            var name = WorkspaceLauncherStore.displayName(of: shortcut)
            if WorkspaceLauncherStore.isGroup(shortcut) {
                name += " ›"
            }
            actions.append(UIAlertAction(title: name, style: .default) { [weak self] _ in
                self?.nextTurn { self?.presentEditLauncherShortcut(path: path, index: index) }
            })
        }
        actions.append(contentsOf: addActions(from: sourceView, path: path))
        presentLauncherSheet(title: "Edit Shortcuts",
                             message: shortcuts.isEmpty
                                 ? "Add a shortcut to run a command, or just open a terminal."
                                 : "Pick a shortcut to rename, change, move, or delete.",
                             actions: actions, from: sourceView, sourceRect: sourceView.bounds)
    }

    private func presentAddLauncherShortcut(path: [Int]) {
        let alert = UIAlertController(title: "Add Shortcut",
                                      message: "A name, and a command to run in a new terminal. Leave it blank for just a terminal, or use a {token} like {clock} or {themes} to open a built-in.",
                                      preferredStyle: .alert)
        alert.addTextField { field in
            field.placeholder = "Name (e.g. skippy or terminal)"
            field.autocapitalizationType = .none
            field.accessibilityIdentifier = "workspace.launcher.name"
        }
        alert.addTextField { field in
            field.placeholder = "Command (e.g. ssh skippy, blank for a shell)"
            field.autocapitalizationType = .none
            field.autocorrectionType = .no
            field.accessibilityIdentifier = "workspace.launcher.command"
        }
        alert.addAction(UIAlertAction(title: "Save", style: .default) { [weak alert] _ in
            let nameField = alert?.textFields?[0].text ?? ""
            let command = alert?.textFields?[1].text ?? ""
            guard !nameField.isEmpty || !command.isEmpty else { return }
            let name = nameField.isEmpty ? command : nameField
            WorkspaceLauncherStore.mutate(atPath: path) { $0.append(["name": name, "command": command]) }
        })
        alert.addAction(UIAlertAction(title: "Cancel", style: .cancel))
        present(alert, animated: true)
    }

    private func presentAddLauncherGroup(path: [Int]) {
        let alert = UIAlertController(title: "Add Group",
                                      message: "A group holds its own list of shortcuts, such as \"Remote Login\" holding several hosts. Add shortcuts to it after creating it.",
                                      preferredStyle: .alert)
        alert.addTextField { field in
            field.placeholder = "Name (e.g. Remote Login)"
            field.autocapitalizationType = .words
        }
        alert.addAction(UIAlertAction(title: "Save", style: .default) { [weak alert] _ in
            guard let name = alert?.textFields?[0].text, !name.isEmpty else { return }
            WorkspaceLauncherStore.mutate(atPath: path) { $0.append(["name": name, "children": [Any]()]) }
        })
        alert.addAction(UIAlertAction(title: "Cancel", style: .cancel))
        present(alert, animated: true)
    }

    private func presentAddLauncherBuiltIn(from sourceView: UIView, path: [Int]) {
        let actions = WorkspaceLauncherStore.builtIns.map { builtIn in
            UIAlertAction(title: builtIn.name, style: .default) { _ in
                WorkspaceLauncherStore.mutate(atPath: path) { $0.append(["name": builtIn.name, "command": builtIn.command]) }
            }
        }
        presentLauncherSheet(title: "Add Built-in", message: "Open a built-in tool straight from the Launcher.",
                             actions: actions, from: sourceView, sourceRect: sourceView.bounds)
    }

    func presentEditLauncherShortcut(path: [Int], index: Int) {
        let shortcuts = WorkspaceLauncherStore.array(atPath: path)
        guard shortcuts.indices.contains(index) else { return }
        let shortcut = shortcuts[index]
        let group = WorkspaceLauncherStore.isGroup(shortcut)
        let alert = UIAlertController(title: group ? "Edit Group" : "Edit Shortcut",
                                      message: group
                                          ? "Rename, move, or delete this group. Go into it from the list to edit what's inside."
                                          : "Leave the command blank to just open a terminal.",
                                      preferredStyle: .alert)
        alert.addTextField { field in
            field.placeholder = "Name"
            field.text = shortcut["name"] as? String
            field.autocapitalizationType = .none
        }
        if !group {
            alert.addTextField { field in
                field.placeholder = "Command (blank for a shell)"
                field.text = shortcut["command"] as? String
                field.autocapitalizationType = .none
                field.autocorrectionType = .no
            }
        }
        alert.addAction(UIAlertAction(title: "Save", style: .default) { [weak alert] _ in
            let nameField = alert?.textFields?[0].text ?? ""
            let command = group ? "" : (alert?.textFields?[1].text ?? "")
            guard !nameField.isEmpty || !command.isEmpty else { return }
            let name = nameField.isEmpty ? command : nameField
            WorkspaceLauncherStore.mutate(atPath: path) { level in
                guard level.indices.contains(index) else { return }
                if group {
                    level[index] = ["name": name, "children": WorkspaceLauncherStore.children(of: level[index])]
                } else {
                    level[index] = ["name": name, "command": command]
                }
            }
        })
        if index > 0 {
            alert.addAction(UIAlertAction(title: "Move Up", style: .default) { _ in
                WorkspaceLauncherStore.mutate(atPath: path) { level in
                    guard index > 0, level.indices.contains(index) else { return }
                    level.swapAt(index, index - 1)
                }
            })
        }
        if index + 1 < shortcuts.count {
            alert.addAction(UIAlertAction(title: "Move Down", style: .default) { _ in
                WorkspaceLauncherStore.mutate(atPath: path) { level in
                    guard level.indices.contains(index + 1) else { return }
                    level.swapAt(index, index + 1)
                }
            })
        }
        let count = WorkspaceLauncherStore.children(of: shortcut).count
        let deleteTitle = group && count > 0
            ? (count == 1 ? "Delete Group and 1 Item" : "Delete Group and \(count) Items")
            : "Delete"
        alert.addAction(UIAlertAction(title: deleteTitle, style: .destructive) { _ in
            WorkspaceLauncherStore.mutate(atPath: path) { level in
                if level.indices.contains(index) {
                    level.remove(at: index)
                }
            }
        })
        alert.addAction(UIAlertAction(title: "Cancel", style: .cancel))
        present(alert, animated: true)
    }
}
