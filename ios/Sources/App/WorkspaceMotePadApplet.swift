// Copyright 2026 Mike Miller
// SPDX-License-Identifier: Apache-2.0
//
// Ported by its author from iSH-AOK's Workspace (app/WorkspaceViewController.m at
// 0f75fee1c) and licensed to PSCAL under the Apache License 2.0.

import UIKit

private enum WorkspaceMotePadBrowserMode {
    case open, save
}

/// A small file browser for Open and Save As, folders first, then files. It returns a chosen
/// file, or a chosen folder and a typed name. Going into a folder pushes a fresh one of these
/// onto its navigation controller.
@MainActor
private final class WorkspaceMotePadFileBrowserViewController: UITableViewController {
    private let mode: WorkspaceMotePadBrowserMode
    private let directory: String
    private let suggestedName: String?
    private var completion: ((String?) -> Void)?
    private var entries: [WorkspaceMotePadEntry] = []
    private var showsParent = false
    // Discards a listing that has been overtaken.
    private var loadGeneration = 0

    init(mode: WorkspaceMotePadBrowserMode, directory: String, suggestedName: String?, completion: ((String?) -> Void)?) {
        self.mode = mode
        self.directory = directory.isEmpty ? "/" : directory
        self.suggestedName = suggestedName
        self.completion = completion
        super.init(style: .plain)
    }

    @available(*, unavailable)
    required init?(coder: NSCoder) {
        fatalError("init(coder:) is not used")
    }

    override func viewDidLoad() {
        super.viewDidLoad()
        let name = (directory as NSString).lastPathComponent
        title = name.isEmpty ? "/" : name
        navigationItem.prompt = directory
        let cancel = UIBarButtonItem(barButtonSystemItem: .cancel, target: self, action: #selector(cancelTapped))
        cancel.accessibilityIdentifier = "workspace.motepad.browser.cancel"
        navigationItem.leftBarButtonItem = cancel
        if mode == .save {
            let saveHere = UIBarButtonItem(title: "Save Here", style: .done, target: self, action: #selector(saveHereTapped))
            saveHere.accessibilityIdentifier = "workspace.motepad.browser.savehere"
            navigationItem.rightBarButtonItem = saveHere
        }
        tableView.accessibilityIdentifier = "workspace.motepad.browser.list"
        reload()
    }

    /// While a listing is out the table shows a spinner and rejects taps, so a half-loaded
    /// folder can't be used. Cancel stays live, so there's always a way out.
    private func reload() {
        showsParent = directory != "/"
        loadGeneration += 1
        let generation = loadGeneration
        setLoading(true)
        WorkspaceMotePadStore.shared.listDirectory(directory) { [weak self] result in
            guard let self, self.loadGeneration == generation else { return }
            self.setLoading(false)
            switch result {
            case .success(let entries):
                self.entries = entries
            case .failure(let error):
                self.entries = []
                let label = UILabel()
                label.text = error.localizedDescription
                label.textAlignment = .center
                label.textColor = .secondaryLabel
                label.numberOfLines = 0
                self.tableView.backgroundView = label
            }
            self.tableView.reloadData()
        }
    }

    private func setLoading(_ loading: Bool) {
        if loading {
            let spinner = UIActivityIndicatorView(style: .medium)
            spinner.startAnimating()
            tableView.backgroundView = spinner
        } else {
            tableView.backgroundView = nil
        }
        tableView.isUserInteractionEnabled = !loading
        navigationItem.rightBarButtonItem?.isEnabled = !loading
    }

    @objc private func cancelTapped() {
        finish(path: nil)
    }

    /// Dismisses first and runs the completion after: it may present an alert, and presenting
    /// while this is still going away silently fails, which would make a failed Save As look
    /// as if it had worked.
    private func finish(path: String?) {
        let completion = self.completion
        self.completion = nil
        dismiss(animated: true) {
            completion?(path)
        }
    }

    private func push(directory: String) {
        let child = WorkspaceMotePadFileBrowserViewController(mode: mode, directory: directory, suggestedName: suggestedName, completion: completion)
        navigationController?.pushViewController(child, animated: true)
    }

    @objc private func saveHereTapped() {
        promptForFilename(suggestedName, message: nil)
    }

    private func promptForFilename(_ initialName: String?, message: String?) {
        let alert = UIAlertController(title: "Save As", message: message ?? directory, preferredStyle: .alert)
        alert.addTextField { field in
            field.placeholder = "filename.txt"
            field.text = initialName
            field.autocapitalizationType = .none
            field.autocorrectionType = .no
            field.clearButtonMode = .whileEditing
            field.accessibilityIdentifier = "workspace.motepad.browser.filename"
        }
        alert.addAction(UIAlertAction(title: "Save", style: .default) { [weak self, weak alert] _ in
            guard let self else { return }
            let name = (alert?.textFields?.first?.text ?? "").trimmingCharacters(in: .whitespacesAndNewlines)
            // After the alert has finished going away: dismissals overlapping at two levels of
            // the hierarchy can cancel each other, and a second alert can't present yet.
            DispatchQueue.main.async { [weak self] in
                self?.filenameChosen(name)
            }
        })
        alert.addAction(UIAlertAction(title: "Cancel", style: .cancel))
        present(alert, animated: true)
    }

    private func filenameChosen(_ name: String) {
        if name.isEmpty || name.contains("/") || name.contains("\0") || name == "." || name == ".." {
            // Not swallowed: the prompt comes back with the reason.
            promptForFilename(name, message: "Enter a file name without “/” (and not “.” or “..”).")
            return
        }
        let path = WorkspaceFileService.join(directory, name)
        guard entries.contains(where: { $0.name.caseInsensitiveCompare(name) == .orderedSame }) else {
            finish(path: path)
            return
        }
        let alert = UIAlertController(title: "Replace “\(name)”?", message: "A file with that name is already in this folder.", preferredStyle: .alert)
        alert.addAction(UIAlertAction(title: "Replace", style: .destructive) { [weak self] _ in
            DispatchQueue.main.async { [weak self] in
                self?.finish(path: path)
            }
        })
        alert.addAction(UIAlertAction(title: "Cancel", style: .cancel))
        present(alert, animated: true)
    }

    override func tableView(_ tableView: UITableView, numberOfRowsInSection section: Int) -> Int {
        entries.count + (showsParent ? 1 : 0)
    }

    override func tableView(_ tableView: UITableView, cellForRowAt indexPath: IndexPath) -> UITableViewCell {
        let cell = tableView.dequeueReusableCell(withIdentifier: "motepad.browser")
            ?? UITableViewCell(style: .default, reuseIdentifier: "motepad.browser")
        if showsParent && indexPath.row == 0 {
            cell.textLabel?.text = ".."
            cell.imageView?.image = UIImage(systemName: "arrow.turn.left.up")
            cell.accessoryType = .disclosureIndicator
            cell.textLabel?.textColor = .label
            cell.accessibilityIdentifier = "workspace.motepad.browser.parent"
            return cell
        }
        let entry = entries[indexPath.row - (showsParent ? 1 : 0)]
        cell.textLabel?.text = entry.name
        cell.accessibilityIdentifier = "workspace.motepad.browser.row.\(entry.name)"
        if entry.kind == .directory {
            cell.imageView?.image = UIImage(systemName: "folder")
            cell.accessoryType = .disclosureIndicator
            cell.textLabel?.textColor = .label
        } else {
            cell.imageView?.image = UIImage(systemName: "doc.text")
            cell.accessoryType = .none
            cell.textLabel?.textColor = entry.kind == .file ? .label : .secondaryLabel
        }
        return cell
    }

    override func tableView(_ tableView: UITableView, didSelectRowAt indexPath: IndexPath) {
        tableView.deselectRow(at: indexPath, animated: true)
        if showsParent && indexPath.row == 0 {
            push(directory: WorkspaceFileService.parent(of: directory))
            return
        }
        let entry = entries[indexPath.row - (showsParent ? 1 : 0)]
        switch entry.kind {
        case .directory:
            push(directory: entry.path)
        case .file:
            if mode == .open {
                finish(path: entry.path)
            } else {
                // Tapping an existing file offers its name, for an overwrite.
                promptForFilename(entry.name, message: nil)
            }
        case .other:
            // A socket or FIFO can't open as text, and opening one can block.
            break
        }
    }
}

/// Newlines are what Ln counts and what the gutter numbers, so a wrapped line is one line.
private enum WorkspaceMotePadLines {
    static func newlineCount(in text: NSString, upTo index: Int) -> Int {
        let limit = min(index, text.length)
        let chunk = 16384
        var buffer = [unichar](repeating: 0, count: chunk)
        var count = 0
        var position = 0
        while position < limit {
            let length = min(chunk, limit - position)
            text.getCharacters(&buffer, range: NSRange(location: position, length: length))
            for offset in 0..<length where buffer[offset] == 10 {
                count += 1
            }
            position += length
        }
        return count
    }
}

/// The left-margin line numbers. iOS has no ruler view, so this is a plain view driven by the
/// editor's layout manager: one number per hard line, at that line's first fragment. The editor
/// asks it to redraw on a scroll, an edit, a wrap or a font change.
@MainActor
private final class WorkspaceMotePadGutterView: UIView {
    weak var textView: UITextView?
    var numberColor: UIColor?
    var separatorColor: UIColor?

    override init(frame: CGRect) {
        super.init(frame: frame)
        contentMode = .redraw
        isUserInteractionEnabled = false
    }

    @available(*, unavailable)
    required init?(coder: NSCoder) {
        fatalError("init(coder:) is not used")
    }

    private var gutterFont: UIFont {
        let size = textView?.font?.pointSize ?? 12
        return .monospacedDigitSystemFont(ofSize: max(9, size - 1), weight: .regular)
    }

    private func drawNumber(_ number: Int, atY y: CGFloat, font: UIFont) {
        let attributes: [NSAttributedString.Key: Any] = [.font: font, .foregroundColor: numberColor ?? UIColor.secondaryLabel]
        let text = String(number) as NSString
        let size = text.size(withAttributes: attributes)
        text.draw(at: CGPoint(x: bounds.width - size.width - 6, y: y), withAttributes: attributes)
    }

    override func draw(_ rect: CGRect) {
        guard let textView else { return }
        let hair = 1 / max(1, traitCollection.displayScale)
        (separatorColor ?? UIColor(white: 0.5, alpha: 0.35)).setFill()
        UIRectFill(CGRect(x: bounds.width - hair, y: 0, width: hair, height: bounds.height))

        let font = gutterFont
        let insetTop = textView.textContainerInset.top
        let offsetY = textView.contentOffset.y
        let text: NSString = textView.textStorage.mutableString
        if text.length == 0 {
            drawNumber(1, atY: insetTop - offsetY, font: font)
            return
        }
        let layout = textView.layoutManager
        let container = textView.textContainer
        let visible = CGRect(x: 0, y: offsetY - insetTop, width: container.size.width, height: textView.bounds.height)
        let glyphs = layout.glyphRange(forBoundingRect: visible, in: container)
        guard glyphs.length > 0 else { return }

        let firstCharacter = layout.characterIndexForGlyph(at: glyphs.location)
        var paragraph = 1 + WorkspaceMotePadLines.newlineCount(in: text, upTo: firstCharacter)
        var first = true
        layout.enumerateLineFragments(forGlyphRange: glyphs) { fragment, _, _, lineGlyphs, _ in
            let character = layout.characterIndexForGlyph(at: lineGlyphs.location)
            if character == 0 || text.character(at: character - 1) == 10 {
                // Fragments come in document order, so this moves past the last paragraph.
                if !first {
                    paragraph += 1
                }
                self.drawNumber(paragraph, atY: fragment.minY + insetTop - offsetY, font: font)
            }
            first = false
        }
    }
}

/// One line of a MotePad menu. `checked` is nil for a row that isn't a toggle.
private struct WorkspaceMotePadMenuRow {
    let title: String
    let identifier: String
    let shortcut: String?
    let checked: Bool?
    let action: Selector
}

@MainActor
private final class WorkspaceMotePadMenuRowControl: UIControl {
    var highlightColor: UIColor = .clear

    override var isHighlighted: Bool {
        didSet {
            backgroundColor = isHighlighted ? highlightColor : .clear
        }
    }
}

/// A themed popover sized to fit its widest row. A native pull-down menu is at least about 250
/// points wide on iPad, which leaves short menus such as File and Help mostly empty.
@MainActor
private final class WorkspaceMotePadPopupMenuController: UIViewController, UIPopoverPresentationControllerDelegate {
    private let sections: [[WorkspaceMotePadMenuRow]]
    private let colors: WorkspaceThemeColors
    private weak var target: NSObject?
    private var actions: [Selector] = []

    init(sections: [[WorkspaceMotePadMenuRow]], colors: WorkspaceThemeColors, target: NSObject) {
        self.sections = sections
        self.colors = colors
        self.target = target
        super.init(nibName: nil, bundle: nil)
    }

    @available(*, unavailable)
    required init?(coder: NSCoder) {
        fatalError("init(coder:) is not used")
    }

    override func viewDidLoad() {
        super.viewDidLoad()
        let highlight = colors.accent.withAlphaComponent(0.22)
        view.backgroundColor = colors.card

        let titleFont = UIFont.systemFont(ofSize: 16)
        let shortcutFont = UIFont.systemFont(ofSize: 15)

        // Measure the columns, so shortcuts right-align and the popover is as wide as it needs.
        var maxTitle: CGFloat = 0
        var maxShortcut: CGFloat = 0
        var anyCheckable = false
        for row in sections.joined() {
            maxTitle = max(maxTitle, (row.title as NSString).size(withAttributes: [.font: titleFont]).width)
            if let shortcut = row.shortcut {
                maxShortcut = max(maxShortcut, (shortcut as NSString).size(withAttributes: [.font: shortcutFont]).width)
            }
            if row.checked != nil {
                anyCheckable = true
            }
        }
        let inset: CGFloat = 14
        let gap: CGFloat = maxShortcut > 0 ? 28 : 0
        let checkWidth: CGFloat = anyCheckable ? 18 : 0
        let checkGap: CGFloat = anyCheckable ? 8 : 0
        let rowHeight: CGFloat = 34
        let separatorHeight: CGFloat = 9
        let pad: CGFloat = 6
        let wanted = inset + ceil(maxTitle) + gap + ceil(maxShortcut) + checkGap + checkWidth + inset
        let contentWidth = max(150, min(wanted, 320))

        let stack = UIStackView()
        stack.axis = .vertical
        stack.spacing = 0
        stack.translatesAutoresizingMaskIntoConstraints = false
        view.addSubview(stack)
        // Centred, with a minimum pad each side. A popover's content is often a little taller
        // than its preferred size, and this splits the surplus instead of pooling it below.
        NSLayoutConstraint.activate([
            stack.centerYAnchor.constraint(equalTo: view.centerYAnchor),
            stack.topAnchor.constraint(greaterThanOrEqualTo: view.topAnchor, constant: pad),
            stack.bottomAnchor.constraint(lessThanOrEqualTo: view.bottomAnchor, constant: -pad),
            stack.leadingAnchor.constraint(equalTo: view.leadingAnchor),
            stack.trailingAnchor.constraint(equalTo: view.trailingAnchor),
        ])

        var rowCount = 0
        for (index, section) in sections.enumerated() {
            if index > 0 {
                let separator = UIView()
                separator.translatesAutoresizingMaskIntoConstraints = false
                separator.heightAnchor.constraint(equalToConstant: separatorHeight).isActive = true
                let hair = UIView()
                hair.translatesAutoresizingMaskIntoConstraints = false
                hair.backgroundColor = colors.stroke
                separator.addSubview(hair)
                NSLayoutConstraint.activate([
                    hair.leadingAnchor.constraint(equalTo: separator.leadingAnchor, constant: inset),
                    hair.trailingAnchor.constraint(equalTo: separator.trailingAnchor, constant: -inset),
                    hair.centerYAnchor.constraint(equalTo: separator.centerYAnchor),
                    hair.heightAnchor.constraint(equalToConstant: 1 / max(1, UIScreen.main.scale)),
                ])
                stack.addArrangedSubview(separator)
            }
            for row in section {
                stack.addArrangedSubview(makeRowView(row, highlight: highlight, titleFont: titleFont, shortcutFont: shortcutFont,
                                                     rowHeight: rowHeight, inset: inset, checkWidth: checkWidth, checkGap: checkGap))
                rowCount += 1
            }
        }
        let separators = max(0, sections.count - 1)
        preferredContentSize = CGSize(width: contentWidth,
                                      height: CGFloat(rowCount) * rowHeight + CGFloat(separators) * separatorHeight + pad * 2)
    }

    private func makeRowView(_ row: WorkspaceMotePadMenuRow, highlight: UIColor, titleFont: UIFont, shortcutFont: UIFont,
                             rowHeight: CGFloat, inset: CGFloat, checkWidth: CGFloat, checkGap: CGFloat) -> UIView {
        let rowView = WorkspaceMotePadMenuRowControl()
        rowView.translatesAutoresizingMaskIntoConstraints = false
        rowView.highlightColor = highlight
        rowView.tag = actions.count
        rowView.heightAnchor.constraint(equalToConstant: rowHeight).isActive = true
        rowView.addTarget(self, action: #selector(rowTapped(_:)), for: .touchUpInside)
        rowView.isAccessibilityElement = true
        rowView.accessibilityLabel = row.title
        rowView.accessibilityIdentifier = "workspace.motepad.\(row.identifier)"
        rowView.accessibilityTraits = row.checked == true ? [.button, .selected] : .button
        actions.append(row.action)

        let titleLabel = UILabel()
        titleLabel.translatesAutoresizingMaskIntoConstraints = false
        titleLabel.text = row.title
        titleLabel.font = titleFont
        titleLabel.textColor = colors.primary
        rowView.addSubview(titleLabel)
        NSLayoutConstraint.activate([
            titleLabel.leadingAnchor.constraint(equalTo: rowView.leadingAnchor, constant: inset),
            titleLabel.centerYAnchor.constraint(equalTo: rowView.centerYAnchor),
        ])

        var check: UIImageView?
        if checkWidth > 0 {
            let image = UIImageView(image: UIImage(systemName: "checkmark"))
            image.translatesAutoresizingMaskIntoConstraints = false
            image.contentMode = .scaleAspectFit
            image.tintColor = colors.accent
            image.isHidden = row.checked != true
            rowView.addSubview(image)
            NSLayoutConstraint.activate([
                image.trailingAnchor.constraint(equalTo: rowView.trailingAnchor, constant: -inset),
                image.centerYAnchor.constraint(equalTo: rowView.centerYAnchor),
                image.widthAnchor.constraint(equalToConstant: checkWidth),
                image.heightAnchor.constraint(equalToConstant: checkWidth),
            ])
            check = image
        }

        if let shortcut = row.shortcut {
            let label = UILabel()
            label.translatesAutoresizingMaskIntoConstraints = false
            label.text = shortcut
            label.font = shortcutFont
            label.textColor = colors.secondary
            label.textAlignment = .right
            rowView.addSubview(label)
            let trailing = check.map { label.trailingAnchor.constraint(equalTo: $0.leadingAnchor, constant: -checkGap) }
                ?? label.trailingAnchor.constraint(equalTo: rowView.trailingAnchor, constant: -inset)
            NSLayoutConstraint.activate([
                trailing,
                label.centerYAnchor.constraint(equalTo: rowView.centerYAnchor),
                label.leadingAnchor.constraint(greaterThanOrEqualTo: titleLabel.trailingAnchor, constant: 12),
            ])
        }
        return rowView
    }

    /// After the menu has gone, so an action that presents something doesn't collide with it.
    @objc private func rowTapped(_ sender: UIControl) {
        let action = sender.tag < actions.count ? actions[sender.tag] : nil
        dismiss(animated: true) { [weak target] in
            if let action {
                _ = target?.perform(action)
            }
        }
    }

    /// Stays a popover even at compact width, on a phone.
    func adaptivePresentationStyle(for controller: UIPresentationController, traitCollection: UITraitCollection) -> UIModalPresentationStyle {
        .none
    }
}

/// Keeps the app awake for a queued write on its way into the background, in place of blocking
/// the main thread until the write is done.
@MainActor
private final class WorkspaceMotePadBackgroundTask {
    private var identifier = UIBackgroundTaskIdentifier.invalid

    init() {
        identifier = UIApplication.shared.beginBackgroundTask(withName: "MotePad draft") { [weak self] in
            self?.end()
        }
    }

    func end() {
        guard identifier != .invalid else { return }
        UIApplication.shared.endBackgroundTask(identifier)
        identifier = .invalid
    }
}

/// A Notepad-style text editor: File, Edit, Format, View and Help menus over a text view, with
/// an optional line-number gutter and a status bar. Text is UTF-8 only, and unsaved text is
/// mirrored to a draft that survives the app being killed.
@MainActor
final class WorkspaceMotePadAppletViewController: WorkspaceAppletViewController, WorkspaceFileOpenable, WorkspaceFocusable,
    WorkspaceStatefulTool, WorkspaceTextScalable, UITextViewDelegate, UIFontPickerViewControllerDelegate {
    /// The editor's size at text scale 1.
    private static let defaultFontSize: CGFloat = 14
    private static let draftDelay: TimeInterval = 2
    private static let diskCheckInterval: TimeInterval = 3

    /// How the document compares with the file at its path.
    private enum DiskState {
        case inStep, changed, deleted
    }

    private let store = WorkspaceMotePadStore.shared
    private let menuBar = UIView()
    private let menuBarSeparator = UIView()
    private var menuButtons: [UIButton] = []
    private let titleLabel = UILabel()
    /// TextKit 1 from the start: the gutter and the wrap switch use its layout manager, and
    /// reaching for that on a TextKit 2 view would rebuild it at run time.
    private let textView: UITextView = {
        if #available(iOS 16.0, *) {
            return UITextView(usingTextLayoutManager: false)
        }
        return UITextView()
    }()
    private let statusBar = UIView()
    private let statusLabel = UILabel()
    private let gutter = WorkspaceMotePadGutterView(frame: .zero)
    private var statusBarHeight: NSLayoutConstraint?
    private var gutterWidth: NSLayoutConstraint?
    private var gutterContentWidth: CGFloat = 0

    /// nil is an untitled document.
    private var documentPath: String?
    private var documentFingerprint: WorkspaceMotePadFingerprint?
    private var documentHasByteOrderMark = false
    /// Moves on with each load and save, so a disk check that was overtaken is ignored.
    private var documentGeneration = 0
    private var lastDiskReading = WorkspaceMotePadDiskReading.unknown
    private var diskState = DiskState.inStep
    private var diskNoticeDue = false
    private var reopenFailure: String?
    private var diskCheckTimer: Timer?

    private var dirty = false
    /// Moves on with each edit, so a save that finishes after one leaves the document dirty.
    private var editGeneration = 0
    private var wordWrap = true
    private var statusBarVisible = true
    private var lineNumbersVisible = false
    private var lastSearch: String?
    private var statusUpdatePending = false
    private var grabbedInitialFocus = false

    private var busyCount = 0
    private var lockCount = 0

    private var draftSlot: String?
    private var draftTimer: Timer?
    private var draftIsStale = false
    private var draftLoadGeneration = 0
    private var draftRestorePending = false
    private var afterDraftRestore: [() -> Void] = []
    private var discardDraftOnLeaving = false

    private var documentString: NSString { textView.textStorage.mutableString }

    private var isFrontmost: Bool {
        guard let host = workspaceHost, let window = host.window(holding: self) else { return false }
        return host.frontmostWindow() === window
    }

    var textViewForTesting: UITextView { textView }
    var documentPathForTesting: String? { documentPath }
    var isDirtyForTesting: Bool { dirty }

    deinit {
        draftTimer?.invalidate()
        diskCheckTimer?.invalidate()
        if let draftSlot {
            WorkspaceMotePadStore.shared.releaseDraftSlot(draftSlot)
        }
    }

    // MARK: - Interface

    override func viewDidLoad() {
        super.viewDidLoad()
        title = "MotePad"
        let content = toolContentView
        let phone = WorkspaceMetrics.usesPhoneLayout

        menuBar.translatesAutoresizingMaskIntoConstraints = false
        content.addSubview(menuBar)

        let menuStack = UIStackView()
        menuStack.translatesAutoresizingMaskIntoConstraints = false
        menuStack.axis = .horizontal
        menuStack.spacing = phone ? 2 : 6
        menuStack.alignment = .center
        menuBar.addSubview(menuStack)

        for name in ["File", "Edit", "Format", "View", "Help"] {
            let button = UIButton(type: .system)
            button.translatesAutoresizingMaskIntoConstraints = false
            button.setTitle(name, for: .normal)
            button.titleLabel?.font = .systemFont(ofSize: phone ? 13 : 14, weight: .medium)
            button.contentEdgeInsets = UIEdgeInsets(top: 4, left: phone ? 6 : 8, bottom: 4, right: phone ? 6 : 8)
            button.accessibilityIdentifier = "workspace.motepad.menu.\(name.lowercased())"
            button.addTarget(self, action: #selector(menuButtonTapped(_:)), for: .touchUpInside)
            menuButtons.append(button)
            menuStack.addArrangedSubview(button)
        }

        titleLabel.translatesAutoresizingMaskIntoConstraints = false
        titleLabel.font = .systemFont(ofSize: phone ? 11 : 12, weight: .semibold)
        titleLabel.textAlignment = .right
        titleLabel.adjustsFontSizeToFitWidth = true
        titleLabel.minimumScaleFactor = 0.7
        titleLabel.setContentHuggingPriority(.defaultLow, for: .horizontal)
        titleLabel.setContentCompressionResistancePriority(.defaultLow, for: .horizontal)
        titleLabel.accessibilityIdentifier = "workspace.motepad.title"
        menuBar.addSubview(titleLabel)

        menuBarSeparator.translatesAutoresizingMaskIntoConstraints = false
        menuBar.addSubview(menuBarSeparator)

        textView.translatesAutoresizingMaskIntoConstraints = false
        textView.delegate = self
        textView.isEditable = true
        textView.font = UIFont(name: "Courier", size: Self.defaultFontSize)
            ?? .monospacedSystemFont(ofSize: Self.defaultFontSize, weight: .regular)
        textView.autocapitalizationType = .none
        textView.autocorrectionType = .no
        textView.smartQuotesType = .no
        textView.smartDashesType = .no
        textView.spellCheckingType = .no
        textView.alwaysBounceVertical = true
        textView.textContainerInset = UIEdgeInsets(top: 8, left: 6, bottom: 8, right: 6)
        textView.accessibilityIdentifier = "workspace.motepad.text"
        content.addSubview(textView)

        // Hidden until the View menu asks for it.
        gutter.translatesAutoresizingMaskIntoConstraints = false
        gutter.textView = textView
        gutter.accessibilityIdentifier = "workspace.motepad.gutter"
        content.addSubview(gutter)

        statusBar.translatesAutoresizingMaskIntoConstraints = false
        content.addSubview(statusBar)

        statusLabel.translatesAutoresizingMaskIntoConstraints = false
        statusLabel.font = .monospacedDigitSystemFont(ofSize: 11, weight: .regular)
        statusLabel.adjustsFontSizeToFitWidth = true
        statusLabel.minimumScaleFactor = 0.7
        statusLabel.accessibilityIdentifier = "workspace.motepad.status"
        statusBar.addSubview(statusLabel)

        let barHeight: CGFloat = phone ? 32 : 34
        let statusHeight = statusBar.heightAnchor.constraint(equalToConstant: 22)
        statusBarHeight = statusHeight
        let gutterWidth = gutter.widthAnchor.constraint(equalToConstant: 0)
        self.gutterWidth = gutterWidth

        // The window is only raised over the keyboard for a terminal, so the editor keeps its
        // caret in view by stopping above the keyboard itself. That gives way before the text
        // view goes altogether, when the keyboard covers nearly all of a low window.
        let restsOnWindow = statusBar.bottomAnchor.constraint(equalTo: content.bottomAnchor)
        restsOnWindow.priority = .defaultLow
        let clearsKeyboard = statusBar.bottomAnchor.constraint(lessThanOrEqualTo: view.keyboardLayoutGuide.topAnchor)
        clearsKeyboard.priority = .defaultHigh

        NSLayoutConstraint.activate([
            menuBar.topAnchor.constraint(equalTo: content.topAnchor),
            menuBar.leadingAnchor.constraint(equalTo: content.leadingAnchor),
            menuBar.trailingAnchor.constraint(equalTo: content.trailingAnchor),
            menuBar.heightAnchor.constraint(equalToConstant: barHeight),

            menuStack.leadingAnchor.constraint(equalTo: menuBar.leadingAnchor, constant: 4),
            menuStack.centerYAnchor.constraint(equalTo: menuBar.centerYAnchor),

            titleLabel.leadingAnchor.constraint(greaterThanOrEqualTo: menuStack.trailingAnchor, constant: 6),
            titleLabel.trailingAnchor.constraint(equalTo: menuBar.trailingAnchor, constant: -8),
            titleLabel.centerYAnchor.constraint(equalTo: menuBar.centerYAnchor),

            menuBarSeparator.leadingAnchor.constraint(equalTo: menuBar.leadingAnchor),
            menuBarSeparator.trailingAnchor.constraint(equalTo: menuBar.trailingAnchor),
            menuBarSeparator.bottomAnchor.constraint(equalTo: menuBar.bottomAnchor),
            menuBarSeparator.heightAnchor.constraint(equalToConstant: 1 / max(1, UIScreen.main.scale)),

            gutter.topAnchor.constraint(equalTo: menuBar.bottomAnchor),
            gutter.leadingAnchor.constraint(equalTo: content.leadingAnchor),
            gutter.bottomAnchor.constraint(equalTo: statusBar.topAnchor),
            gutterWidth,

            textView.topAnchor.constraint(equalTo: menuBar.bottomAnchor),
            textView.leadingAnchor.constraint(equalTo: gutter.trailingAnchor),
            textView.trailingAnchor.constraint(equalTo: content.trailingAnchor),
            textView.bottomAnchor.constraint(equalTo: statusBar.topAnchor),
            textView.heightAnchor.constraint(greaterThanOrEqualToConstant: 44),

            statusBar.leadingAnchor.constraint(equalTo: content.leadingAnchor),
            statusBar.trailingAnchor.constraint(equalTo: content.trailingAnchor),
            statusBar.bottomAnchor.constraint(lessThanOrEqualTo: content.bottomAnchor),
            restsOnWindow,
            clearsKeyboard,
            statusHeight,

            statusLabel.leadingAnchor.constraint(equalTo: statusBar.leadingAnchor, constant: 10),
            statusLabel.trailingAnchor.constraint(equalTo: statusBar.trailingAnchor, constant: -10),
            statusLabel.centerYAnchor.constraint(equalTo: statusBar.centerYAnchor),
        ])

        applyWordWrap()
        updateDocumentTitle()
        updateStatusBar()
        applyStatusBarVisibility()
        updateGutterWidth()
        applyLineNumbersVisibility()
        applyTheme()

        draftSlot = store.claimDraftSlot()
        restoreDraft(thenOpening: nil)
        let center = NotificationCenter.default
        center.addObserver(self, selector: #selector(flushDraft), name: UIApplication.didEnterBackgroundNotification, object: nil)
        center.addObserver(self, selector: #selector(flushDraft), name: UIScene.didEnterBackgroundNotification, object: nil)
        center.addObserver(self, selector: #selector(checkDisk), name: UIApplication.didBecomeActiveNotification, object: nil)
    }

    /// The keyboard as soon as the applet opens, if it's the window in front: a window a restore
    /// puts behind a terminal must not take the keyboard from it.
    override func viewDidAppear(_ animated: Bool) {
        super.viewDidAppear(animated)
        if diskCheckTimer == nil {
            diskCheckTimer = Timer.scheduledTimer(withTimeInterval: Self.diskCheckInterval, repeats: true) { [weak self] _ in
                MainActor.assumeIsolated {
                    self?.checkDisk()
                }
            }
        }
        if !grabbedInitialFocus, isFrontmost {
            grabbedInitialFocus = true
            textView.becomeFirstResponder()
        }
        checkDisk()
    }

    override func viewDidDisappear(_ animated: Bool) {
        super.viewDidDisappear(animated)
        diskCheckTimer?.invalidate()
        diskCheckTimer = nil
    }

    override func viewDidLayoutSubviews() {
        super.viewDidLayoutSubviews()
        if lineNumbersVisible {
            gutter.setNeedsDisplay()
        }
    }

    /// Every way out passes here, including the window's × and leaving Workspace. Those don't
    /// prompt, so the draft is all that protects the text, and text typed less than the debounce
    /// ago has to reach it now.
    override func willMove(toParent parent: UIViewController?) {
        super.willMove(toParent: parent)
        if parent == nil {
            flushDraft()
        }
    }

    override func applyTheme() {
        super.applyTheme()
        let colors = theme
        menuBar.backgroundColor = colors.card.withAlphaComponent(0.96)
        menuBarSeparator.backgroundColor = colors.stroke
        menuButtons.forEach { $0.setTitleColor(colors.primary, for: .normal) }
        titleLabel.textColor = colors.secondary

        textView.backgroundColor = colors.card.withAlphaComponent(0.6)
        textView.textColor = colors.primary
        textView.tintColor = colors.accent
        statusBar.backgroundColor = colors.card.withAlphaComponent(0.96)
        statusLabel.textColor = colors.secondary

        gutter.backgroundColor = colors.card.withAlphaComponent(0.6)
        gutter.numberColor = colors.secondary
        gutter.separatorColor = colors.stroke
        gutter.setNeedsDisplay()
    }

    /// Only the document scales; the menu bar, title and status bar are chrome. `withSize` keeps
    /// the face, so a font picked with Show Fonts survives, and the picker keeps the size.
    override func applyTextScale() {
        super.applyTextScale()
        guard isViewLoaded else { return }
        let size = scaledFontSize(Self.defaultFontSize)
        // Setting the font isn't an edit: no dirty flag, no draft, no undo.
        textView.font = (textView.font ?? .monospacedSystemFont(ofSize: size, weight: .regular)).withSize(size)
        updateGutterWidth()
        gutter.setNeedsDisplay()
        textView.scrollRangeToVisible(textView.selectedRange)
    }

    // MARK: - Focus

    /// A tap, window cycling or a Desktop switch brought this window forward. Workspace then asks
    /// the terminals to take input, and one that hasn't yet heard it lost focus can still do so
    /// and then let go, leaving nobody with the keyboard. So this looks again a moment later.
    func workspaceToolDidBecomeFrontmost() {
        guard isViewLoaded else { return }
        textView.becomeFirstResponder()
        checkDisk()
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.25) { [weak self] in
            MainActor.assumeIsolated {
                guard let self, self.isFrontmost, self.presentedViewController == nil, !self.textView.isFirstResponder else { return }
                self.textView.becomeFirstResponder()
            }
        }
    }

    // MARK: - Menus

    private func row(_ title: String, _ identifier: String, _ shortcut: String?, _ action: Selector, checked: Bool? = nil) -> WorkspaceMotePadMenuRow {
        WorkspaceMotePadMenuRow(title: title, identifier: identifier, shortcut: shortcut, checked: checked, action: action)
    }

    private func menuSections(for title: String) -> [[WorkspaceMotePadMenuRow]] {
        switch title {
        case "File":
            return [
                [row("New", "new", "⌘N", #selector(newDocument)),
                 row("Open…", "open", "⌘O", #selector(openDocument)),
                 row("Save", "save", "⌘S", #selector(saveDocument)),
                 row("Save As…", "saveas", "⇧⌘S", #selector(saveDocumentAs))],
                [row("Page Setup…", "pagesetup", "⇧⌘P", #selector(pageSetup)),
                 row("Print…", "print", "⌘P", #selector(printDocument))],
                [row("Close", "close", "⌘W", #selector(closeDocument)),
                 row("Quit MotePad", "quit", "⌘Q", #selector(quitMotePad))],
            ]
        case "Edit":
            return [
                [row("Undo", "undo", "⌘Z", #selector(undoEdit)),
                 row("Redo", "redo", "⇧⌘Z", #selector(redoEdit))],
                [row("Cut", "cut", "⌘X", #selector(cutSelection)),
                 row("Copy", "copy", "⌘C", #selector(copySelection)),
                 row("Paste", "paste", "⌘V", #selector(pasteClipboard)),
                 row("Delete", "delete", nil, #selector(deleteSelection)),
                 row("Select All", "selectall", "⌘A", #selector(selectEverything))],
                [row("Find…", "find", "⌘F", #selector(showFind)),
                 row("Find Next", "findnext", "⌘G", #selector(findNextMatch)),
                 row("Replace…", "replace", "⇧⌘F", #selector(showReplace)),
                 row("Go to Line…", "gotoline", "⌘L", #selector(showGoToLine)),
                 row("Insert Date and Time", "insertdate", "⌘D", #selector(insertDateAndTime))],
            ]
        case "Format":
            return [
                [row("Word Wrap", "wordwrap", "⌥⌘W", #selector(toggleWordWrap), checked: wordWrap),
                 row("Show Fonts", "fonts", "⌘T", #selector(showFonts))],
            ]
        case "View":
            return [
                [row("Status Bar", "statusbar", "⌥⌘S", #selector(toggleStatusBar), checked: statusBarVisible),
                 row("Line Numbers", "linenumbers", "⌥⌘L", #selector(toggleLineNumbers), checked: lineNumbersVisible)],
                [row("Bigger", "bigger", "⌘+", #selector(biggerText)),
                 row("Smaller", "smaller", "⌘−", #selector(smallerText)),
                 row("Actual Size", "actualsize", "⌘0", #selector(actualSizeText))],
            ]
        case "Help":
            return [[row("MotePad Help", "help", "⌘?", #selector(showHelp))]]
        default:
            return []
        }
    }

    @objc private func menuButtonTapped(_ button: UIButton) {
        openMenu(for: button)
    }

    private func openMenu(for button: UIButton) {
        let sections = menuSections(for: button.title(for: .normal) ?? "")
        guard !sections.isEmpty else { return }
        let menu = WorkspaceMotePadPopupMenuController(sections: sections, colors: theme, target: self)
        menu.modalPresentationStyle = .popover
        if let popover = menu.popoverPresentationController {
            popover.sourceView = button
            popover.sourceRect = button.bounds
            popover.permittedArrowDirections = .up
            popover.delegate = menu
            popover.backgroundColor = theme.card
        }
        present(menu, animated: true)
    }

    private func openMenu(titled title: String) {
        if let button = menuButtons.first(where: { $0.title(for: .normal) == title }) {
            openMenu(for: button)
        }
    }

    @objc private func openFileMenu() { openMenu(titled: "File") }
    @objc private func openEditMenu() { openMenu(titled: "Edit") }
    @objc private func openFormatMenu() { openMenu(titled: "Format") }
    @objc private func openViewMenu() { openMenu(titled: "View") }
    @objc private func openHelpMenu() { openMenu(titled: "Help") }

    // MARK: - Keyboard shortcuts

    private func key(_ input: String, _ flags: UIKeyModifierFlags, _ action: Selector, _ title: String) -> UIKeyCommand {
        let command = UIKeyCommand(input: input, modifierFlags: flags, action: action)
        command.discoverabilityTitle = title
        return command
    }

    private lazy var motePadKeyCommands: [UIKeyCommand] = {
        let shiftCommand: UIKeyModifierFlags = [.command, .shift]
        let optionCommand: UIKeyModifierFlags = [.command, .alternate]
        let controlOption: UIKeyModifierFlags = [.control, .alternate]
        return [
            // Undo, Redo, Cut, Copy, Paste and Select All come from the text view.
            key("n", .command, #selector(newDocument), "New"),
            key("o", .command, #selector(openDocument), "Open…"),
            key("s", .command, #selector(saveDocument), "Save"),
            key("s", shiftCommand, #selector(saveDocumentAs), "Save As…"),
            key("p", .command, #selector(printDocument), "Print…"),
            key("p", shiftCommand, #selector(pageSetup), "Page Setup…"),
            key("w", .command, #selector(closeDocument), "Close"),
            key("q", .command, #selector(quitMotePad), "Quit MotePad"),
            key("f", .command, #selector(showFind), "Find…"),
            key("g", .command, #selector(findNextMatch), "Find Next"),
            key("f", shiftCommand, #selector(showReplace), "Replace…"),
            key("l", .command, #selector(showGoToLine), "Go to Line…"),
            key("d", .command, #selector(insertDateAndTime), "Insert Date and Time"),
            key("w", optionCommand, #selector(toggleWordWrap), "Word Wrap"),
            key("t", .command, #selector(showFonts), "Show Fonts"),
            key("s", optionCommand, #selector(toggleStatusBar), "Status Bar"),
            key("l", optionCommand, #selector(toggleLineNumbers), "Line Numbers"),
            key("?", .command, #selector(showHelp), "MotePad Help"),
            key("f", controlOption, #selector(openFileMenu), "File Menu"),
            key("e", controlOption, #selector(openEditMenu), "Edit Menu"),
            key("o", controlOption, #selector(openFormatMenu), "Format Menu"),
            key("v", controlOption, #selector(openViewMenu), "View Menu"),
            key("h", controlOption, #selector(openHelpMenu), "Help Menu"),
        ]
    }()

    override var keyCommands: [UIKeyCommand]? { motePadKeyCommands }

    // MARK: - Document state

    private var documentDisplayName: String {
        guard let documentPath else { return "Untitled" }
        let name = (documentPath as NSString).lastPathComponent
        return name.isEmpty ? "Untitled" : name
    }

    private func updateDocumentTitle() {
        let name = documentDisplayName
        titleLabel.text = dirty ? "● " + name : name
    }

    private func setDirty(_ value: Bool) {
        guard dirty != value else { return }
        dirty = value
        updateDocumentTitle()
    }

    /// This document's text and what's known of its file, as just read or just written.
    private func noteInStep(path: String?, fingerprint: WorkspaceMotePadFingerprint?) {
        documentPath = path
        documentFingerprint = fingerprint
        documentGeneration &+= 1
        lastDiskReading = fingerprint.map { .present($0) } ?? .unknown
        diskState = .inStep
        diskNoticeDue = false
        reopenFailure = nil
    }

    private func load(text: String, path: String?, fingerprint: WorkspaceMotePadFingerprint?, hasByteOrderMark: Bool) {
        textView.text = text
        noteInStep(path: path, fingerprint: fingerprint)
        documentHasByteOrderMark = hasByteOrderMark
        textView.undoManager?.removeAllActions()
        draftIsStale = false
        setDirty(false)
        updateDocumentTitle()
        updateStatusBar()
        if lineNumbersVisible {
            updateGutterWidth()
            gutter.setNeedsDisplay()
        }
        // The document is clean now, so this clears any stale draft.
        autosaveDraftNow()
    }

    // MARK: - Drafts

    private func currentDraft() -> WorkspaceMotePadDraft {
        WorkspaceMotePadDraft(text: textView.text ?? "", path: documentPath, date: Date(),
                              hasByteOrderMark: documentHasByteOrderMark, base: documentFingerprint)
    }

    private func scheduleDraftAutosave() {
        draftTimer?.invalidate()
        draftTimer = Timer.scheduledTimer(withTimeInterval: Self.draftDelay, repeats: false) { [weak self] _ in
            MainActor.assumeIsolated {
                self?.autosaveDraftNow()
            }
        }
    }

    private func autosaveDraftNow() {
        draftTimer?.invalidate()
        draftTimer = nil
        guard let draftSlot else { return }
        guard dirty else {
            store.removeDraft(slot: draftSlot)
            return
        }
        draftIsStale = false
        store.writeDraft(currentDraft(), slot: draftSlot)
    }

    /// The app is going to the background, where the debounce won't fire and a kill is routine,
    /// or the window is leaving. The write is queued like any other, with a background task to
    /// see it through.
    @objc private func flushDraft() {
        draftTimer?.invalidate()
        draftTimer = nil
        guard let draftSlot, !discardDraftOnLeaving else { return }
        guard dirty else {
            store.removeDraft(slot: draftSlot)
            return
        }
        guard draftIsStale else { return }
        draftIsStale = false
        let task = WorkspaceMotePadBackgroundTask()
        store.writeDraft(currentDraft(), slot: draftSlot) {
            task.end()
        }
    }

    /// The draft comes back for a window that hasn't been typed into. A draft is never dropped
    /// because the file moved on: it's the only copy of that text, so the person hears about the
    /// file instead. With nothing restored, `path` is the document to reopen.
    private func restoreDraft(thenOpening path: String?) {
        guard let draftSlot else {
            if let path {
                loadDocument(at: path, quietly: true)
            }
            return
        }
        draftLoadGeneration &+= 1
        let generation = draftLoadGeneration
        draftRestorePending = true
        store.loadDraft(slot: draftSlot) { [weak self] loaded in
            guard let self, self.draftLoadGeneration == generation else { return }
            self.draftRestorePending = false
            if let loaded {
                self.apply(draft: loaded.draft, reading: loaded.reading)
            }
            if let path, !self.dirty, self.documentPath == nil, self.documentString.length == 0 {
                self.loadDocument(at: path, quietly: true)
            }
            let waiting = self.afterDraftRestore
            self.afterDraftRestore = []
            waiting.forEach { $0() }
        }
    }

    private func apply(draft: WorkspaceMotePadDraft, reading: WorkspaceMotePadDiskReading) {
        // Typing has already started, so it isn't stomped.
        guard !dirty, documentString.length == 0 else { return }
        textView.text = draft.text
        noteInStep(path: draft.path, fingerprint: draft.base)
        lastDiskReading = .unknown
        documentHasByteOrderMark = draft.hasByteOrderMark
        textView.undoManager?.removeAllActions()
        draftIsStale = false
        setDirty(true)
        updateDocumentTitle()
        updateStatusBar()
        if lineNumbersVisible {
            updateGutterWidth()
            gutter.setNeedsDisplay()
        }
        if draft.path != nil {
            note(reading: reading)
        }
    }

    // MARK: - Saved state

    /// What this editor needs to come back as itself. The draft is queued rather than written
    /// here: the layout saves this on the main thread, and it asks often.
    func workspaceToolStateForSaving() -> [String: Any]? {
        var state: [String: Any] = ["dirty": dirty]
        if let draftSlot {
            if dirty && draftIsStale {
                autosaveDraftNow()
            }
            state["draftSlot"] = draftSlot
        }
        if let documentPath {
            state["path"] = documentPath
        }
        return state
    }

    func workspaceRestoreToolState(_ state: [String: Any]) {
        loadViewIfNeeded()
        // viewDidLoad claimed a slot by number. Swap to the one this window actually wrote, if
        // nothing else holds it, and give the guessed one back for the next window. A window with
        // unsaved text of its own keeps its slot, or its autosave would write over that draft.
        if !dirty, let slot = state["draftSlot"] as? String, !slot.isEmpty, slot != draftSlot, store.claimDraftSlot(named: slot) {
            if let draftSlot {
                store.releaseDraftSlot(draftSlot)
            }
            draftSlot = slot
        }
        restoreDraft(thenOpening: state["path"] as? String)
    }

    // MARK: - File actions

    @objc private func newDocument() {
        promptToSaveIfDirty { [weak self] in
            self?.load(text: "", path: nil, fingerprint: nil, hasByteOrderMark: false)
        }
    }

    /// A file operation in flight. Saving takes a snapshot, so only opening has to lock the
    /// editor, and a save doesn't make the keyboard go and come back.
    private func beginFileOperation(_ status: String, locksEditor: Bool) {
        busyCount += 1
        if locksEditor {
            lockCount += 1
            textView.isEditable = false
        }
        statusLabel.text = status
    }

    private func endFileOperation(lockedEditor: Bool) {
        busyCount = max(0, busyCount - 1)
        if lockedEditor {
            lockCount = max(0, lockCount - 1)
            if lockCount == 0 {
                textView.isEditable = true
                if isFrontmost, presentedViewController == nil {
                    if textView.isFirstResponder {
                        textView.reloadInputViews()
                    } else {
                        textView.becomeFirstResponder()
                    }
                }
            }
        }
        if busyCount == 0 {
            updateStatusBar()
        }
    }

    /// `quietly` is for a document a restore reopens: not being able to isn't worth an alert at
    /// launch, so it goes in the status bar.
    private func loadDocument(at path: String, quietly: Bool = false) {
        let path = WorkspaceMotePadStore.normalizedPath(path)
        beginFileOperation("Opening…", locksEditor: true)
        documentGeneration &+= 1
        store.readText(path) { [weak self] result in
            guard let self else { return }
            self.endFileOperation(lockedEditor: true)
            let name = (path as NSString).lastPathComponent
            switch result {
            case .success(let document):
                self.load(text: document.text, path: path, fingerprint: document.fingerprint, hasByteOrderMark: document.hasByteOrderMark)
            case .failure(let error) where quietly:
                self.reopenFailure = "couldn’t reopen “\(name)”: \(error.localizedDescription)"
                self.updateStatusBar()
            case .failure(let error):
                self.presentError(error, title: "Couldn’t Open “\(name)”")
            }
        }
    }

    @objc private func openDocument() {
        promptToSaveIfDirty { [weak self] in
            self?.presentBrowser(mode: .open, suggestedName: nil) { path in
                guard let path else { return }
                self?.loadDocument(at: path)
            }
        }
    }

    /// `ws open motepad <path>` and the File Manager arrive here, with a path already known.
    func workspaceOpenFile(atPath path: String) {
        loadViewIfNeeded()
        promptToSaveIfDirty { [weak self] in
            self?.loadDocument(at: path)
        }
    }

    @objc func saveDocument() {
        save(completion: nil)
    }

    private func save(completion: ((Bool) -> Void)?) {
        guard let path = documentPath else {
            saveAs(completion: completion)
            return
        }
        let known = documentFingerprint
        beginFileOperation("Saving…", locksEditor: false)
        store.diskReading(path) { [weak self] reading in
            guard let self else {
                completion?(false)
                return
            }
            self.endFileOperation(lockedEditor: false)
            if case .present(let current) = reading, current != known {
                self.confirmOverwrite(path: path, completion: completion)
            } else {
                self.write(to: path, completion: completion)
            }
        }
    }

    /// The file isn't the one this text came from, so saving would lose someone's work.
    private func confirmOverwrite(path: String, completion: ((Bool) -> Void)?) {
        let alert = UIAlertController(title: "“\(documentDisplayName)” Changed on Disk",
                                      message: "Something else changed this file after you opened it. Saving here replaces that version.",
                                      preferredStyle: .alert)
        alert.addAction(UIAlertAction(title: "Replace", style: .destructive) { [weak self] _ in
            self?.write(to: path, completion: completion)
        })
        alert.addAction(UIAlertAction(title: "Save As…", style: .default) { [weak self] _ in
            self?.saveAs(completion: completion)
        })
        alert.addAction(UIAlertAction(title: "Cancel", style: .cancel) { _ in
            completion?(false)
        })
        presentOnTop(alert)
    }

    private func saveAs(completion: ((Bool) -> Void)?) {
        presentBrowser(mode: .save, suggestedName: documentDisplayName) { [weak self] path in
            guard let self, let path else {
                completion?(false)
                return
            }
            self.write(to: path, completion: completion)
        }
    }

    @objc private func saveDocumentAs() {
        saveAs(completion: nil)
    }

    /// Queued: the write runs off the main thread, and the completion on it says how it went.
    private func write(to path: String, completion: ((Bool) -> Void)?) {
        let savedGeneration = editGeneration
        beginFileOperation("Saving…", locksEditor: false)
        documentGeneration &+= 1
        store.writeText(textView.text ?? "", hasByteOrderMark: documentHasByteOrderMark, to: path) { [weak self] result in
            guard let self else {
                completion?(false)
                return
            }
            self.endFileOperation(lockedEditor: false)
            switch result {
            case .failure(let error):
                self.presentSaveFailure(error, path: path)
                completion?(false)
            case .success(let fingerprint):
                self.noteInStep(path: WorkspaceMotePadStore.normalizedPath(path), fingerprint: fingerprint)
                if savedGeneration == self.editGeneration {
                    self.setDirty(false)
                    self.draftIsStale = false
                    self.autosaveDraftNow()
                }
                self.updateDocumentTitle()
                self.updateStatusBar()
                completion?(true)
            }
        }
    }

    private func presentSaveFailure(_ error: Error, path: String) {
        let name = (path as NSString).lastPathComponent
        let alert = UIAlertController(title: "Couldn’t Save “\(name)”",
                                      message: "\(error.localizedDescription)\n\nYour text is still here. Save As can keep it somewhere else.",
                                      preferredStyle: .alert)
        alert.addAction(UIAlertAction(title: "Save As…", style: .default) { [weak self] _ in
            self?.saveAs(completion: nil)
        })
        alert.addAction(UIAlertAction(title: "OK", style: .cancel))
        presentOnTop(alert)
    }

    @objc private func printDocument() {
        let controller = UIPrintInteractionController.shared
        let info = UIPrintInfo.printInfo()
        info.outputType = .general
        info.jobName = documentDisplayName
        controller.printInfo = info
        let formatter = UISimpleTextPrintFormatter(text: textView.text ?? "")
        formatter.font = textView.font
        controller.printFormatter = formatter
        // Valid on both idioms: a pad needs a source, and a phone presents modally regardless.
        let anchor: UIView = menuButtons.first ?? view
        controller.present(from: anchor.bounds, in: anchor, animated: true, completionHandler: nil)
    }

    /// iOS folds paper size and orientation into the print sheet, so this opens the same thing.
    @objc private func pageSetup() {
        printDocument()
    }

    /// A deliberate close, once the person has saved or chosen Don't Save: dropping the draft
    /// stops discarded text coming back in the next window.
    @objc private func closeDocument() {
        promptToSaveIfDirty { [weak self] in
            guard let self else { return }
            self.discardDraftOnLeaving = true
            self.draftTimer?.invalidate()
            self.draftTimer = nil
            if let draftSlot = self.draftSlot {
                self.store.removeDraft(slot: draftSlot)
            }
            self.workspaceHost?.window(holding: self)?.closeHandler?()
        }
    }

    /// iOS has no quit for an applet, so this closes its window.
    @objc private func quitMotePad() {
        closeDocument()
    }

    private func presentBrowser(mode: WorkspaceMotePadBrowserMode, suggestedName: String?, completion: @escaping (String?) -> Void) {
        if let documentPath {
            presentBrowser(mode: mode, directory: WorkspaceFileService.parent(of: documentPath), suggestedName: suggestedName, completion: completion)
            return
        }
        store.defaultDirectory { [weak self] directory in
            self?.presentBrowser(mode: mode, directory: directory, suggestedName: suggestedName, completion: completion)
        }
    }

    private func presentBrowser(mode: WorkspaceMotePadBrowserMode, directory: String, suggestedName: String?, completion: @escaping (String?) -> Void) {
        let browser = WorkspaceMotePadFileBrowserViewController(mode: mode, directory: directory, suggestedName: suggestedName, completion: completion)
        let navigation = UINavigationController(rootViewController: browser)
        navigation.modalPresentationStyle = .formSheet
        navigation.overrideUserInterfaceStyle = WorkspaceTheme.prefersDarkInterface ? .dark : .light
        navigation.view.tintColor = WorkspaceTheme.accent
        presentOnTop(navigation)
    }

    /// Runs `proceed` once the document is safe: clean, saved, or given up on by choice. A draft
    /// still being read counts as a document, so it waits for that first.
    private func promptToSaveIfDirty(then proceed: @escaping () -> Void) {
        if draftRestorePending {
            afterDraftRestore.append { [weak self] in
                self?.promptToSaveIfDirty(then: proceed)
            }
            return
        }
        guard dirty else {
            proceed()
            return
        }
        let alert = UIAlertController(title: "Unsaved Changes", message: "Save changes to \(documentDisplayName)?", preferredStyle: .alert)
        alert.addAction(UIAlertAction(title: "Save", style: .default) { [weak self] _ in
            // Proceeds only if the write actually happened.
            self?.save { saved in
                if saved {
                    proceed()
                }
            }
        })
        alert.addAction(UIAlertAction(title: "Don’t Save", style: .destructive) { _ in
            proceed()
        })
        alert.addAction(UIAlertAction(title: "Cancel", style: .cancel))
        presentOnTop(alert)
    }

    // MARK: - Changes on disk

    /// Another program can change or delete the file at any moment, and a shell is usually in
    /// the next window. Asked on a slow timer, on coming to the front, and on the app returning.
    @objc private func checkDisk() {
        guard let documentPath, busyCount == 0, isViewLoaded, view.window != nil else { return }
        let generation = documentGeneration
        store.diskReading(documentPath) { [weak self] reading in
            guard let self, self.documentGeneration == generation, self.busyCount == 0 else { return }
            self.note(reading: reading)
        }
    }

    private func note(reading: WorkspaceMotePadDiskReading) {
        if reading != .unknown, reading != lastDiskReading {
            lastDiskReading = reading
            switch reading {
            case .missing:
                diskState = .deleted
                // The text on screen is now the only copy, so closing has to ask, and it needs
                // a draft like any unsaved text.
                if !dirty {
                    setDirty(true)
                }
                draftIsStale = true
                scheduleDraftAutosave()
            case .present(let current):
                diskState = current == documentFingerprint ? .inStep : .changed
            case .unknown:
                break
            }
            diskNoticeDue = diskState != .inStep
            updateStatusBar()
        }
        presentDiskNoticeIfDue()
    }

    /// Once for each thing that happened, and not until this window is the one being looked at.
    /// The status bar says so meanwhile.
    private func presentDiskNoticeIfDue() {
        guard diskNoticeDue, busyCount == 0, isViewLoaded, view.window != nil, isFrontmost, presentedViewController == nil else { return }
        let alert: UIAlertController
        switch diskState {
        case .inStep:
            return
        case .changed:
            alert = UIAlertController(title: "“\(documentDisplayName)” Changed on Disk",
                                      message: "Something else changed this file. Your text here stays as it is unless you reload.",
                                      preferredStyle: .alert)
            alert.addAction(UIAlertAction(title: "Reload from Disk", style: dirty ? .destructive : .default) { [weak self] _ in
                guard let self, let path = self.documentPath else { return }
                self.loadDocument(at: path)
            })
            alert.addAction(UIAlertAction(title: "Keep My Text", style: .cancel))
        case .deleted:
            alert = UIAlertController(title: "“\(documentDisplayName)” Was Deleted",
                                      message: "The file is gone, but your text is still here. Save puts it back.",
                                      preferredStyle: .alert)
            alert.addAction(UIAlertAction(title: "Save", style: .default) { [weak self] _ in
                self?.saveDocument()
            })
            alert.addAction(UIAlertAction(title: "Keep Editing", style: .cancel))
        }
        diskNoticeDue = false
        present(alert, animated: true)
    }

    // MARK: - Edit actions

    @objc private func undoEdit() { textView.undoManager?.undo() }
    @objc private func redoEdit() { textView.undoManager?.redo() }
    @objc private func cutSelection() { textView.cut(nil) }
    @objc private func copySelection() { textView.copy(nil) }
    @objc private func pasteClipboard() { textView.paste(nil) }
    @objc private func selectEverything() { textView.selectAll(nil) }

    /// The current selection. With none there's nothing to do: the Delete key deletes a character.
    @objc private func deleteSelection() {
        if textView.selectedRange.length > 0 {
            textView.deleteBackward()
        }
    }

    @objc private func showFind() {
        if #available(iOS 16.0, *) {
            textView.isFindInteractionEnabled = true
            textView.findInteraction?.presentFindNavigator(showingReplace: false)
            return
        }
        // Before iOS 16: a one-shot forward find from the caret.
        let alert = UIAlertController(title: "Find", message: nil, preferredStyle: .alert)
        alert.addTextField { field in
            field.placeholder = "Search"
            field.autocapitalizationType = .none
        }
        alert.addAction(UIAlertAction(title: "Find Next", style: .default) { [weak self, weak alert] _ in
            self?.findNext(alert?.textFields?.first?.text ?? "")
        })
        alert.addAction(UIAlertAction(title: "Cancel", style: .cancel))
        presentOnTop(alert)
    }

    @objc private func findNextMatch() {
        if let lastSearch, !lastSearch.isEmpty {
            findNext(lastSearch)
        } else {
            showFind()
        }
    }

    @objc private func showReplace() {
        if #available(iOS 16.0, *) {
            textView.isFindInteractionEnabled = true
            textView.findInteraction?.presentFindNavigator(showingReplace: true)
            return
        }
        // No system replace before iOS 16.
        showFind()
    }

    private func findNext(_ needle: String) {
        guard !needle.isEmpty else { return }
        lastSearch = needle
        let haystack = documentString
        var from = NSMaxRange(textView.selectedRange)
        if from > haystack.length {
            from = 0
        }
        var found = haystack.range(of: needle, options: [], range: NSRange(location: from, length: haystack.length - from))
        if found.location == NSNotFound {
            // Around to the top.
            found = haystack.range(of: needle, options: [], range: NSRange(location: 0, length: haystack.length))
        }
        guard found.location != NSNotFound else { return }
        textView.selectedRange = found
        textView.scrollRangeToVisible(found)
        textView.becomeFirstResponder()
    }

    @objc private func showGoToLine() {
        let alert = UIAlertController(title: "Go to Line", message: nil, preferredStyle: .alert)
        alert.addTextField { field in
            field.placeholder = "Line number"
            field.keyboardType = .numberPad
        }
        alert.addAction(UIAlertAction(title: "Go", style: .default) { [weak self, weak alert] _ in
            self?.goToLine(Int(alert?.textFields?.first?.text ?? "") ?? 0)
        })
        alert.addAction(UIAlertAction(title: "Cancel", style: .cancel))
        presentOnTop(alert)
    }

    private func goToLine(_ line: Int) {
        let wanted = max(1, line)
        let text = documentString
        var index = 0
        var current = 1
        while current < wanted && index < text.length {
            let newline = text.range(of: "\n", options: .literal, range: NSRange(location: index, length: text.length - index))
            if newline.location == NSNotFound {
                index = text.length
                break
            }
            index = NSMaxRange(newline)
            current += 1
        }
        let target = NSRange(location: index, length: 0)
        textView.selectedRange = target
        textView.scrollRangeToVisible(target)
        textView.becomeFirstResponder()
        updateStatusBar()
    }

    @objc private func insertDateAndTime() {
        let formatter = DateFormatter()
        formatter.dateStyle = .medium
        formatter.timeStyle = .short
        if !textView.isFirstResponder {
            textView.becomeFirstResponder()
        }
        // Through the text view, so undo and dirty tracking fire.
        textView.insertText(formatter.string(from: Date()))
    }

    // MARK: - Format and View actions

    @objc private func toggleWordWrap() {
        wordWrap.toggle()
        applyWordWrap()
    }

    private func applyWordWrap() {
        let container = textView.textContainer
        if wordWrap {
            container.widthTracksTextView = true
            // Tracking only acts on the next change of bounds, and this may not be followed by one.
            let insets = textView.textContainerInset
            container.size = CGSize(width: max(0, textView.bounds.width - insets.left - insets.right), height: .greatestFiniteMagnitude)
            textView.contentOffset.x = 0
        } else {
            // Long lines run off to the right, and the text view scrolls across to them.
            container.widthTracksTextView = false
            container.size = CGSize(width: 1.0e7, height: .greatestFiniteMagnitude)
        }
        gutter.setNeedsDisplay()
    }

    @objc private func toggleStatusBar() {
        statusBarVisible.toggle()
        applyStatusBarVisibility()
    }

    private func applyStatusBarVisibility() {
        statusBar.isHidden = !statusBarVisible
        statusBarHeight?.constant = statusBarVisible ? 22 : 0
    }

    @objc private func toggleLineNumbers() {
        lineNumbersVisible.toggle()
        updateGutterWidth()
        applyLineNumbersVisibility()
    }

    private func applyLineNumbersVisibility() {
        gutter.isHidden = !lineNumbersVisible
        gutterWidth?.constant = lineNumbersVisible ? gutterContentWidth : 0
        gutter.setNeedsDisplay()
    }

    /// As wide as the document's largest line number, and always room for two digits, so a short
    /// file doesn't reflow on its tenth line.
    private func updateGutterWidth() {
        let text = documentString
        var remaining = WorkspaceMotePadLines.newlineCount(in: text, upTo: text.length) + 1
        var digits = 1
        while remaining >= 10 {
            digits += 1
            remaining /= 10
        }
        digits = max(digits, 2)
        let font = UIFont.monospacedDigitSystemFont(ofSize: max(9, (textView.font?.pointSize ?? 13) - 1), weight: .regular)
        let digitWidth = ("0" as NSString).size(withAttributes: [.font: font]).width
        gutterContentWidth = ceil(digitWidth * CGFloat(digits)) + 12
        if lineNumbersVisible {
            gutterWidth?.constant = gutterContentWidth
        }
    }

    @objc private func biggerText() { stepTextScale(1) }
    @objc private func smallerText() { stepTextScale(-1) }
    @objc private func actualSizeText() { stepTextScale(0) }

    private func stepTextScale(_ step: Int) {
        let next = WorkspaceTextScale.stepped(workspaceTextScale, step: step)
        workspaceTextScale = next
    }

    @objc private func showFonts() {
        let configuration = UIFontPickerViewController.Configuration()
        configuration.includeFaces = true
        let picker = UIFontPickerViewController(configuration: configuration)
        picker.delegate = self
        picker.overrideUserInterfaceStyle = WorkspaceTheme.prefersDarkInterface ? .dark : .light
        present(picker, animated: true)
    }

    func fontPickerViewControllerDidPickFont(_ viewController: UIFontPickerViewController) {
        if let descriptor = viewController.selectedFontDescriptor {
            let size = textView.font?.pointSize ?? Self.defaultFontSize
            textView.font = UIFont(descriptor: descriptor, size: size)
        }
        if lineNumbersVisible {
            updateGutterWidth()
            gutter.setNeedsDisplay()
        }
        viewController.dismiss(animated: true)
    }

    // MARK: - Help and errors

    @objc private func showHelp() {
        let alert = UIAlertController(title: "MotePad",
                                      message: "A Notepad-style text editor for the PSCAL Workspace.\n\nInspired by MotePad / TinyRetroPad. It reads and writes UTF-8 text files, the same ones the shell sees.",
                                      preferredStyle: .alert)
        alert.addAction(UIAlertAction(title: "OK", style: .default))
        presentOnTop(alert)
    }

    private func presentError(_ error: Error, title: String) {
        let alert = UIAlertController(title: title, message: error.localizedDescription, preferredStyle: .alert)
        alert.addAction(UIAlertAction(title: "OK", style: .default))
        presentOnTop(alert)
    }

    /// Above whatever is up, so a failure's message can't be lost to something else on screen.
    private func presentOnTop(_ controller: UIViewController) {
        guard view.window != nil else { return }
        var top: UIViewController = self
        while let next = top.presentedViewController, !next.isBeingDismissed {
            top = next
        }
        top.present(controller, animated: true)
    }

    // MARK: - Status bar

    private func scheduleStatusUpdate() {
        guard !statusUpdatePending else { return }
        // Typing changes the text and the selection together, and each count walks the text.
        statusUpdatePending = true
        DispatchQueue.main.async { [weak self] in
            self?.statusUpdatePending = false
            self?.updateStatusBar()
        }
    }

    private func updateStatusBar() {
        guard busyCount == 0 else { return }
        let text = documentString
        let caret = min(textView.selectedRange.location, text.length)
        let line = 1 + WorkspaceMotePadLines.newlineCount(in: text, upTo: caret)
        let newline = text.range(of: "\n", options: [.backwards, .literal], range: NSRange(location: 0, length: caret))
        let lineStart = newline.location == NSNotFound ? 0 : NSMaxRange(newline)
        // In characters as a person counts them, so an emoji is one column. Past a screenful the
        // cost isn't worth it, and the UTF-16 count is close enough.
        let span = caret - lineStart
        let column = span <= 4096 ? text.substring(with: NSRange(location: lineStart, length: span)).count : span
        var status = "Ln \(line), Col \(column + 1)"
        switch diskState {
        case .inStep:
            if let reopenFailure {
                status += " · " + reopenFailure
            }
        case .changed:
            status += " · changed on disk"
        case .deleted:
            status += " · deleted from disk"
        }
        statusLabel.text = status
    }

    // MARK: - UITextViewDelegate

    func textViewDidChange(_ textView: UITextView) {
        editGeneration &+= 1
        draftIsStale = true
        setDirty(true)
        scheduleStatusUpdate()
        if lineNumbersVisible {
            updateGutterWidth()
            gutter.setNeedsDisplay()
        }
        scheduleDraftAutosave()
    }

    func textViewDidChangeSelection(_ textView: UITextView) {
        scheduleStatusUpdate()
    }

    func scrollViewDidScroll(_ scrollView: UIScrollView) {
        if lineNumbersVisible {
            gutter.setNeedsDisplay()
        }
    }
}

extension WorkspaceMotePadAppletViewController: WorkspaceKeyClaiming {
    /// New, Show Fonts and Close are MotePad's own here, and the arrows move the caret.
    /// Ctrl-Option with an arrow still switches Desktop.
    func workspaceClaimsKey(input: String, keyCode: UIKeyboardHIDUsage, modifiers: UIKeyModifierFlags) -> Bool {
        guard modifiers == .command else { return false }
        return ["n", "t", "w"].contains(input) || keyCode == .keyboardLeftArrow || keyCode == .keyboardRightArrow
    }
}
