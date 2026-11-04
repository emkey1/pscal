// Copyright 2026 Mike Miller
// SPDX-License-Identifier: Apache-2.0
//
// Ported by its author from iSH-AOK's Workspace (app/WorkspaceViewController.m at
// 0f75fee1c) and licensed to PSCAL under the Apache License 2.0.

import UIKit

/// The applets a Workspace window can hold, by the identifier saved layouts and `ws` use.
@MainActor
enum WorkspaceToolRegistry {
    static let desktops = "workspaces"
    static let launcher = "launcher"
    static let quickActions = "shortcuts"
    static let sessions = "sessions"
    static let themes = "themes"
    static let clock = "clock"
    static let monitor = "monitor"
    static let networks = "networks"
    static let logs = "status"
    static let settings = "settings"
    static let diagnostics = "diagnostics"
    // Later stages. Named here so a saved {token} keeps its meaning.
    static let storage = "storage"
    static let fileManager = "filemanager"
    static let motePad = "motepad"
    static let markdown = "markdown"
    static let imageViewer = "imageviewer"
    static let videoPlayer = "videoplayer"
    static let browser = "browser"
    static let music = "audio"
    static let equalizer = "equalizer"

    /// One window, shared by every Desktop, rather than a window per request.
    static func isGlobal(_ identifier: String) -> Bool {
        identifier == desktops || identifier == launcher
    }

    /// Applets where each path gets a window of its own. Any other keeps to one window.
    static func opensFiles(_ identifier: String) -> Bool {
        [fileManager, motePad, markdown, imageViewer, videoPlayer].contains(identifier)
    }

    static func isAvailable(_ identifier: String) -> Bool {
        factories[identifier] != nil
    }

    private static let factories: [String: () -> WorkspaceAppletViewController] = [
        desktops: { WorkspaceDesktopsAppletViewController() },
        launcher: { WorkspaceLauncherAppletViewController() },
        quickActions: { WorkspaceQuickActionsAppletViewController() },
        sessions: { WorkspaceSessionsAppletViewController() },
        themes: { WorkspaceThemesAppletViewController() },
        clock: { WorkspaceClockAppletViewController() },
        monitor: { WorkspaceMonitorAppletViewController() },
        networks: { WorkspaceNetworksAppletViewController() },
        logs: { WorkspaceLogsAppletViewController() },
        settings: { WorkspaceSettingsAppletViewController() },
        diagnostics: { WorkspaceDiagnosticsAppletViewController() },
        fileManager: { WorkspaceFileManagerAppletViewController() },
        motePad: { WorkspaceMotePadAppletViewController() },
        markdown: { WorkspaceMarkdownAppletViewController() },
        imageViewer: { WorkspaceImageViewerAppletViewController() },
        storage: { WorkspaceStorageAppletViewController() },
        browser: { WorkspaceBrowserAppletViewController() },
        videoPlayer: { WorkspaceVideoPlayerAppletViewController() },
        music: { WorkspaceMusicAppletViewController() },
        equalizer: { WorkspaceEqualizerAppletViewController() },
    ]

    static var availableTools: [String] {
        factories.keys.sorted()
    }

    static func makeViewController(for identifier: String) -> WorkspaceAppletViewController? {
        factories[identifier]?()
    }

    /// The Applets menu. A group with nothing available isn't shown.
    static var menuGroups: [(title: String, message: String, items: [String])] {
        let groups: [(title: String, message: String, items: [String])] = [
            ("Workspace", "Launchers and workspace-wide controls.",
             [desktops, launcher, quickActions, browser, music, equalizer, motePad, fileManager, sessions, themes]),
            ("Media", "Document and image viewers.", [markdown, imageViewer, videoPlayer]),
            ("Status", "Live clocks and runtime summaries.", [clock, monitor, networks, logs]),
            ("Storage", "What the app's folders hold.", [storage]),
            ("Support", "Settings and diagnostics tools.", [settings, diagnostics]),
        ]
        return groups.compactMap { group in
            let items = group.items.filter(isAvailable)
            return items.isEmpty ? nil : (group.title, group.message, items)
        }
    }

    static func title(for identifier: String) -> String {
        switch identifier {
        case desktops: return "Desktops"
        case launcher: return "Launcher"
        case quickActions: return "Quick Actions"
        case sessions: return "Sessions"
        case themes: return "Themes"
        case clock: return "Clock"
        case monitor: return "Monitor"
        case networks: return "Networks"
        case logs: return "Logs"
        case settings: return "Settings"
        case diagnostics: return "Diagnostics"
        case fileManager: return "File Manager"
        case motePad: return "MotePad"
        case markdown: return "Markdown"
        case imageViewer: return "Image Viewer"
        case storage: return "Storage"
        case browser: return "Browser"
        case videoPlayer: return "Video Player"
        case music: return "Music"
        case equalizer: return "Equalizer"
        default: return identifier
        }
    }

    /// Big enough for the applet's default display without clipping. Placement clamps it to
    /// the usable area, so a width too wide for a small phone shrinks to fit.
    static func preferredSize(for identifier: String) -> CGSize {
        let phone = WorkspaceMetrics.usesPhoneLayout
        switch identifier {
        case desktops:
            return phone ? CGSize(width: 240, height: 200) : CGSize(width: 280, height: 240)
        case launcher:
            return WorkspaceLauncherMetrics.contentSize(itemCount: WorkspaceLauncherStore.shortcuts.count, showsBackRow: false, textScale: 1)
        case quickActions:
            return phone ? CGSize(width: 312, height: 300) : CGSize(width: 400, height: 296)
        case sessions:
            return phone ? CGSize(width: 332, height: 300) : CGSize(width: 460, height: 340)
        case themes:
            return phone ? CGSize(width: 360, height: 620) : CGSize(width: 640, height: 760)
        case clock:
            return phone ? CGSize(width: 220, height: 140) : CGSize(width: 240, height: 148)
        case monitor:
            return WorkspaceMonitorAppletViewController.windowSize(textScale: 1)
        case networks:
            return phone ? CGSize(width: 328, height: 188) : CGSize(width: 360, height: 206)
        case logs:
            return phone ? CGSize(width: 340, height: 248) : CGSize(width: 460, height: 300)
        case settings:
            return phone ? CGSize(width: 376, height: 620) : CGSize(width: 560, height: 620)
        case diagnostics:
            return phone ? CGSize(width: 352, height: 620) : CGSize(width: 620, height: 640)
        case fileManager:
            return phone ? CGSize(width: 340, height: 460) : CGSize(width: 720, height: 480)
        case motePad:
            return phone ? CGSize(width: 340, height: 460) : CGSize(width: 620, height: 560)
        case markdown:
            return phone ? CGSize(width: 340, height: 460) : CGSize(width: 620, height: 640)
        case imageViewer:
            return phone ? CGSize(width: 340, height: 420) : CGSize(width: 620, height: 520)
        case storage:
            return phone ? CGSize(width: 336, height: 420) : CGSize(width: 520, height: 500)
        case browser:
            return phone ? CGSize(width: 352, height: 520) : CGSize(width: 620, height: 420)
        case videoPlayer:
            return phone ? CGSize(width: 340, height: 260) : CGSize(width: 720, height: 460)
        case music:
            return phone ? CGSize(width: 252, height: 510) : CGSize(width: 212, height: 470)
        case equalizer:
            return CGSize(width: 460, height: 300)
        default:
            return phone ? CGSize(width: 320, height: 300) : CGSize(width: 460, height: 420)
        }
    }

    static func minimumSize(for identifier: String) -> CGSize {
        let phone = WorkspaceMetrics.usesPhoneLayout
        switch identifier {
        case desktops:
            return phone ? CGSize(width: 200, height: 120) : CGSize(width: 240, height: 140)
        case launcher:
            return phone ? CGSize(width: 168, height: 80) : CGSize(width: 200, height: 96)
        case quickActions:
            return phone ? CGSize(width: 260, height: 148) : CGSize(width: 300, height: 164)
        case sessions:
            return phone ? CGSize(width: 280, height: 176) : CGSize(width: 340, height: 208)
        case themes:
            return phone ? CGSize(width: 320, height: 420) : CGSize(width: 520, height: 560)
        case clock:
            // Below this the date and zone lines overflow the card whatever the fonts do.
            return CGSize(width: 180, height: 124)
        case monitor:
            return phone ? CGSize(width: 280, height: 144) : CGSize(width: 300, height: 150)
        case networks:
            return phone ? CGSize(width: 280, height: 150) : CGSize(width: 300, height: 156)
        case logs:
            return phone ? CGSize(width: 300, height: 220) : CGSize(width: 360, height: 220)
        case settings, diagnostics:
            return CGSize(width: 320, height: 420)
        case fileManager:
            return phone ? CGSize(width: 280, height: 300) : CGSize(width: 420, height: 320)
        case motePad:
            return phone ? CGSize(width: 280, height: 300) : CGSize(width: 360, height: 300)
        case markdown:
            return phone ? CGSize(width: 260, height: 260) : CGSize(width: 360, height: 320)
        case imageViewer:
            return phone ? CGSize(width: 240, height: 240) : CGSize(width: 320, height: 280)
        case storage:
            return phone ? CGSize(width: 288, height: 188) : CGSize(width: 360, height: 220)
        case browser:
            return phone ? CGSize(width: 300, height: 184) : CGSize(width: 360, height: 240)
        case videoPlayer:
            return phone ? CGSize(width: 240, height: 180) : CGSize(width: 320, height: 220)
        case music:
            return phone ? CGSize(width: 252, height: 390) : CGSize(width: 212, height: 360)
        case equalizer:
            return CGSize(width: 340, height: 240)
        default:
            return CGSize(width: 220, height: 160)
        }
    }
}

/// One row per Desktop, the active one highlighted: tap to jump, touch and hold to rename,
/// a lock so it can't be removed, and × to remove it. Save and Restore sit above the list.
@MainActor
final class WorkspaceDesktopsAppletViewController: WorkspaceAppletViewController {
    private let scrollView = UIScrollView()
    private let contentStack = UIStackView()
    private let rowsStack = UIStackView()
    private let saveButton = UIButton(type: .system)
    private let restoreButton = UIButton(type: .system)
    private var observer: NSObjectProtocol?

    deinit {
        if let observer {
            NotificationCenter.default.removeObserver(observer)
        }
    }

    override func viewDidLoad() {
        super.viewDidLoad()
        scrollView.translatesAutoresizingMaskIntoConstraints = false
        scrollView.alwaysBounceVertical = true
        toolContentView.addSubview(scrollView)

        contentStack.translatesAutoresizingMaskIntoConstraints = false
        contentStack.axis = .vertical
        contentStack.spacing = 6
        scrollView.addSubview(contentStack)

        rowsStack.axis = .vertical
        rowsStack.spacing = 6

        let layoutRow = UIStackView()
        layoutRow.axis = .horizontal
        layoutRow.distribution = .fillEqually
        layoutRow.spacing = 6
        configureIconButton(saveButton, symbol: "square.and.arrow.down", label: "Save Desktops",
                            hint: "Saves which windows are open, where, and on which Desktop",
                            action: #selector(saveTapped))
        saveButton.accessibilityIdentifier = "workspace.desktops.save"
        layoutRow.addArrangedSubview(saveButton)
        configureIconButton(restoreButton, symbol: "arrow.clockwise", label: "Restore Desktops",
                            hint: "Puts the windows back as they were last saved",
                            action: #selector(restoreTapped))
        restoreButton.accessibilityIdentifier = "workspace.desktops.restore"
        layoutRow.addArrangedSubview(restoreButton)
        contentStack.addArrangedSubview(layoutRow)
        contentStack.addArrangedSubview(rowsStack)

        NSLayoutConstraint.activate([
            scrollView.topAnchor.constraint(equalTo: toolContentView.topAnchor),
            scrollView.leadingAnchor.constraint(equalTo: toolContentView.leadingAnchor),
            scrollView.trailingAnchor.constraint(equalTo: toolContentView.trailingAnchor),
            scrollView.bottomAnchor.constraint(equalTo: toolContentView.bottomAnchor),
            contentStack.topAnchor.constraint(equalTo: scrollView.contentLayoutGuide.topAnchor, constant: 6),
            contentStack.leadingAnchor.constraint(equalTo: scrollView.frameLayoutGuide.leadingAnchor, constant: 6),
            contentStack.trailingAnchor.constraint(equalTo: scrollView.frameLayoutGuide.trailingAnchor, constant: -6),
            contentStack.bottomAnchor.constraint(equalTo: scrollView.contentLayoutGuide.bottomAnchor, constant: -6),
        ])

        observer = NotificationCenter.default.addObserver(forName: .workspaceDesktopsDidChange, object: nil, queue: .main) { [weak self] _ in
            MainActor.assumeIsolated {
                self?.rebuild()
            }
        }
        rebuild()
    }

    override func viewWillAppear(_ animated: Bool) {
        super.viewWillAppear(animated)
        rebuild()
    }

    override func themeDidChange() {
        super.themeDidChange()
        rebuild()
    }

    private func configureIconButton(_ button: UIButton, symbol: String, label: String, hint: String, action: Selector) {
        button.setImage(UIImage(systemName: symbol), for: .normal)
        button.accessibilityLabel = label
        button.accessibilityHint = hint
        button.layer.cornerRadius = 12
        button.heightAnchor.constraint(equalToConstant: WorkspaceMetrics.usesPhoneLayout ? 36 : 40).isActive = true
        button.addTarget(self, action: action, for: .touchUpInside)
    }

    /// Lists the Desktops again, and sizes the window to fit them.
    private func rebuild() {
        guard let host = workspaceHost else { return }
        for view in rowsStack.arrangedSubviews {
            rowsStack.removeArrangedSubview(view)
            view.removeFromSuperview()
        }
        let count = max(1, host.desktopCount)
        for index in 0..<count {
            rowsStack.addArrangedSubview(row(index: index, active: index == host.activeDesktopIndex, removable: count > 1, host: host))
        }
        let add = UIButton(type: .system)
        add.setTitle("New Desktop", for: .normal)
        add.titleLabel?.font = .systemFont(ofSize: WorkspaceMetrics.usesPhoneLayout ? 14 : 16, weight: .semibold)
        add.layer.cornerRadius = WorkspaceMetrics.usesPhoneLayout ? 9 : 12
        add.layer.borderWidth = 1
        add.layer.borderColor = WorkspaceTheme.accent.withAlphaComponent(0.4).cgColor
        add.heightAnchor.constraint(equalToConstant: WorkspaceMetrics.usesPhoneLayout ? 30 : 40).isActive = true
        add.accessibilityIdentifier = "workspace.desktops.new"
        add.addTarget(self, action: #selector(addTapped), for: .touchUpInside)
        rowsStack.addArrangedSubview(add)

        // Green means what is on screen is what is saved: a Save that works would otherwise
        // look exactly like one that does nothing.
        let saved = host.arrangementIsSaved
        saveButton.tintColor = saved ? .systemGreen : theme.accent
        saveButton.accessibilityValue = saved ? "Saved" : "Not saved"
        restoreButton.tintColor = theme.accent
        add.tintColor = theme.accent

        // After the host's own placement, which would otherwise undo it.
        DispatchQueue.main.async { [weak self] in
            guard let self, let host = self.workspaceHost else { return }
            host.autosizeDesktopsWindow(contentHeight: self.contentStack.systemLayoutSizeFitting(UIView.layoutFittingCompressedSize).height + 12)
        }
    }

    private func row(index: Int, active: Bool, removable: Bool, host: WorkspaceViewController) -> UIView {
        let colors = theme
        let accent = colors.accent
        let locked = host.isDesktopLocked(index)
        let name = host.nameForDesktop(index)

        let row = UIStackView()
        row.axis = .horizontal
        row.spacing = 6

        let lock = UIButton(type: .system)
        lock.tag = index
        let symbols = UIImage.SymbolConfiguration(pointSize: 13, weight: .semibold)
        lock.setImage(UIImage(systemName: locked ? "lock.fill" : "lock.open", withConfiguration: symbols), for: .normal)
        lock.tintColor = locked ? accent : colors.secondary
        // The first Desktop is always protected, so its lock shows closed and doesn't toggle.
        lock.isEnabled = index != 0
        lock.accessibilityLabel = index == 0 ? "\(name) is protected" : (locked ? "Unlock \(name)" : "Lock \(name)")
        lock.setContentHuggingPriority(.required, for: .horizontal)
        lock.widthAnchor.constraint(equalToConstant: 28).isActive = true
        lock.addTarget(self, action: #selector(lockTapped(_:)), for: .touchUpInside)
        row.addArrangedSubview(lock)

        let jump = UIButton(type: .system)
        jump.tag = index
        jump.contentHorizontalAlignment = .left
        jump.titleEdgeInsets = UIEdgeInsets(top: 0, left: 12, bottom: 0, right: 12)
        jump.titleLabel?.font = .systemFont(ofSize: 15, weight: active ? .semibold : .regular)
        jump.titleLabel?.lineBreakMode = .byTruncatingTail
        jump.layer.cornerRadius = 10
        jump.layer.borderWidth = 1
        jump.layer.borderColor = (active ? accent : colors.stroke).cgColor
        // The active Desktop reads by its border and fill, with its label measured against
        // that fill: the accent drawn on a wash of the same accent is hard to read.
        let fill = WorkspaceTheme.wash(accent, alpha: 0.22, over: colors.backgroundBottom)
        jump.backgroundColor = active ? fill : nil
        jump.setTitle(name, for: .normal)
        jump.setTitleColor(active ? WorkspaceTheme.onSurface(fill) : colors.primary, for: .normal)
        jump.accessibilityTraits = active ? [.button, .selected] : .button
        jump.accessibilityIdentifier = "workspace.desktops.row.\(index)"
        jump.heightAnchor.constraint(equalToConstant: WorkspaceMetrics.usesPhoneLayout ? 30 : 38).isActive = true
        jump.addTarget(self, action: #selector(jumpTapped(_:)), for: .touchUpInside)
        // Touch and hold to rename. The press only begins the gesture; a plain tap still jumps.
        jump.addGestureRecognizer(UILongPressGestureRecognizer(target: self, action: #selector(renamePressed(_:))))
        row.addArrangedSubview(jump)

        // The × is hidden while the Desktop is locked; unlock to remove it.
        if removable && !locked {
            let remove = UIButton(type: .system)
            remove.tag = index
            let symbols = UIImage.SymbolConfiguration(pointSize: 11, weight: .semibold)
            remove.setImage(UIImage(systemName: "xmark", withConfiguration: symbols), for: .normal)
            remove.tintColor = .systemRed
            remove.accessibilityLabel = "Remove \(name)"
            remove.setContentHuggingPriority(.required, for: .horizontal)
            remove.widthAnchor.constraint(equalToConstant: 28).isActive = true
            remove.addTarget(self, action: #selector(removeTapped(_:)), for: .touchUpInside)
            row.addArrangedSubview(remove)
        }
        return row
    }

    @objc private func saveTapped() {
        workspaceHost?.saveArrangement()
    }

    @objc private func restoreTapped() {
        workspaceHost?.restoreArrangementFromButton()
    }

    @objc private func addTapped() {
        workspaceHost?.createNewDesktop()
    }

    @objc private func jumpTapped(_ sender: UIButton) {
        workspaceHost?.switchToDesktop(sender.tag)
    }

    @objc private func lockTapped(_ sender: UIButton) {
        workspaceHost?.toggleDesktopLock(sender.tag)
    }

    @objc private func removeTapped(_ sender: UIButton) {
        workspaceHost?.removeDesktop(sender.tag)
    }

    @objc private func renamePressed(_ recognizer: UILongPressGestureRecognizer) {
        guard recognizer.state == .began, let index = recognizer.view?.tag, let host = workspaceHost,
              index < host.desktopCount else { return }
        let positional = "Desktop \(index + 1)"
        let alert = UIAlertController(title: "Rename Desktop",
                                      message: "Leave it empty to go back to \"\(positional)\".",
                                      preferredStyle: .alert)
        alert.addTextField { field in
            field.text = host.customNameForDesktop(index)
            field.placeholder = positional
            field.autocapitalizationType = .words
            field.clearButtonMode = .whileEditing
            field.returnKeyType = .done
        }
        let save = UIAlertAction(title: "Save", style: .default) { [weak alert, weak host] _ in
            host?.setName(alert?.textFields?.first?.text ?? "", forDesktop: index)
        }
        alert.addAction(UIAlertAction(title: "Cancel", style: .cancel))
        alert.addAction(save)
        alert.preferredAction = save
        host.present(alert, animated: true)
    }
}

extension Notification.Name {
    static let workspaceDesktopsDidChange = Notification.Name("WorkspaceDesktopsDidChange")
}
