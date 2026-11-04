// Copyright 2026 Mike Miller
// SPDX-License-Identifier: Apache-2.0
//
// Ported by its author from iSH-AOK's Workspace (app/WorkspaceMarkdownViewer.m and
// app/MarkdownRenderer.m at 0f75fee1c) and licensed to PSCAL under the Apache License 2.0.

import UIKit

/// The fonts and colours a render bakes in. A theme or text size change renders again.
private struct WorkspaceMarkdownStyle: @unchecked Sendable {
    let baseFont: UIFont
    let text: UIColor
    let secondary: UIColor
    let codeBackground: UIColor?
    let link: UIColor
}

/// Where a delimiter next occurs in one run of text. A run's searches only move forward, so
/// the last answer stays good until the scan passes it, which keeps a line of nothing but `[`
/// linear rather than quadratic.
private struct WorkspaceMarkdownFinder {
    private let needle: [UInt16]
    private var known = false
    private var position: Int?

    init(_ needle: String) {
        self.needle = Array(needle.utf16)
    }

    mutating func next(in text: [UInt16], from start: Int) -> Int? {
        if known {
            guard let position else { return nil }
            if position >= start {
                return position
            }
        }
        known = true
        position = nil
        var index = start
        let last = text.count - needle.count
        while index <= last {
            if text[index] == needle[0] && (needle.count == 1 || text[index + 1] == needle[1]) {
                position = index
                return index
            }
            index += 1
        }
        return nil
    }
}

/// A small, dependency-free Markdown to attributed string renderer: headings, emphasis, inline
/// code, fenced code, blockquotes, lists, links, rules and pipe tables. It runs off the main
/// thread, since a large document takes a while. The terminal's `md` command is separate.
private enum WorkspaceMarkdownRenderer {
    private struct InlineStyle {
        let color: UIColor
        let codeFont: UIFont
        let codeColor: UIColor
        let codeBackground: UIColor?
        let link: UIColor
    }

    /// Past this many characters a table's columns aren't padded to line up: padding every row
    /// to the widest of every column can turn a few megabytes into terabytes.
    private static let tablePaddingLimit = 4_000_000
    private static let deepestListLevel = 12
    private static let horizontalRules: Set<String> = ["---", "***", "___", "- - -", "* * *"]

    private static func font(_ font: UIFont, adding traits: UIFontDescriptor.SymbolicTraits) -> UIFont {
        let base = font.fontDescriptor
        guard let descriptor = base.withSymbolicTraits(base.symbolicTraits.union(traits)) else { return font }
        return UIFont(descriptor: descriptor, size: font.pointSize)
    }

    private static func isAlphanumeric(_ unit: UInt16) -> Bool {
        Unicode.Scalar(unit).map { CharacterSet.alphanumerics.contains($0) } ?? false
    }

    /// A file's bytes as text: UTF-8 first, UTF-16 if it says so, and Latin-1 for the rest,
    /// where every byte is something.
    static func decode(_ data: Data) -> String {
        if data.starts(with: [0xFF, 0xFE]) || data.starts(with: [0xFE, 0xFF]), let text = String(data: data, encoding: .utf16) {
            return text
        }
        var text = String(data: data, encoding: .utf8) ?? String(data: data, encoding: .isoLatin1) ?? ""
        if text.unicodeScalars.first == "\u{FEFF}" {
            text.unicodeScalars.removeFirst()
        }
        return text
    }

    /// Splits a pipe-table row into trimmed cells, dropping the empty boundary cells a leading
    /// or trailing `|` produces.
    private static func tableCells(_ row: String) -> [String] {
        var trimmed = row.trimmingCharacters(in: .whitespaces)
        if trimmed.hasPrefix("|") {
            trimmed.removeFirst()
        }
        if trimmed.hasSuffix("|") {
            trimmed.removeLast()
        }
        return trimmed.components(separatedBy: "|").map { $0.trimmingCharacters(in: .whitespaces) }
    }

    /// A table's second line is the header separator: only pipes, colons, dashes and
    /// whitespace, with at least one dash.
    private static func isTableSeparator(_ line: String) -> Bool {
        let trimmed = line.trimmingCharacters(in: .whitespaces)
        guard !trimmed.isEmpty, trimmed.contains("-") else { return false }
        return trimmed.utf8.allSatisfy { $0 == 0x7C || $0 == 0x3A || $0 == 0x2D || $0 == 0x20 || $0 == 0x09 }
    }

    /// Emphasis (`**`, `*`, `__`, `_`), inline code and links, recursing for nested emphasis.
    /// Underscore emphasis is suppressed inside a word, so snake_case and __dunder__ names
    /// survive. Each level of emphasis removes its delimiter from what it recurses on, so the
    /// depth stays small whatever the input.
    private static func appendInline(_ text: [UInt16], to out: NSMutableAttributedString, font: UIFont, style: InlineStyle) {
        let n = text.count
        var codeEnd = WorkspaceMarkdownFinder("`")
        var boldStarEnd = WorkspaceMarkdownFinder("**")
        var boldUnderscoreEnd = WorkspaceMarkdownFinder("__")
        var italicStarEnd = WorkspaceMarkdownFinder("*")
        var italicUnderscoreEnd = WorkspaceMarkdownFinder("_")
        var bracketEnd = WorkspaceMarkdownFinder("]")
        var parenEnd = WorkspaceMarkdownFinder(")")
        var plainStart = 0

        func substring(_ start: Int, _ end: Int) -> String {
            String(decoding: text[start..<end], as: UTF16.self)
        }
        func flushPlain(_ end: Int) {
            guard end > plainStart else { return }
            out.append(NSAttributedString(string: substring(plainStart, end), attributes: [.font: font, .foregroundColor: style.color]))
        }

        var i = 0
        while i < n {
            let c = text[i]
            let star = c == 0x2A
            let underscore = c == 0x5F

            // Inline code.
            if c == 0x60, let close = codeEnd.next(in: text, from: i + 1), close > i + 1 {
                flushPlain(i)
                var attributes: [NSAttributedString.Key: Any] = [.font: style.codeFont, .foregroundColor: style.codeColor]
                if let background = style.codeBackground {
                    attributes[.backgroundColor] = background
                }
                out.append(NSAttributedString(string: substring(i + 1, close), attributes: attributes))
                i = close + 1
                plainStart = i
                continue
            }

            if star || underscore {
                let boundaryOK = !(underscore && i > 0 && isAlphanumeric(text[i - 1]))

                // Bold.
                if i + 1 < n && text[i + 1] == c {
                    let found = underscore ? boldUnderscoreEnd.next(in: text, from: i + 2) : boldStarEnd.next(in: text, from: i + 2)
                    if boundaryOK, let close = found, close > i + 2 {
                        let after = close + 2
                        if !(underscore && after < n && isAlphanumeric(text[after])) {
                            flushPlain(i)
                            appendInline(Array(text[(i + 2)..<close]), to: out, font: self.font(font, adding: .traitBold), style: style)
                            i = after
                            plainStart = i
                            continue
                        }
                    }
                }

                // Italic.
                if boundaryOK && i + 1 < n && text[i + 1] != 0x20 {
                    let found = underscore ? italicUnderscoreEnd.next(in: text, from: i + 1) : italicStarEnd.next(in: text, from: i + 1)
                    if let close = found, close > i + 1 {
                        let after = close + 1
                        if !(underscore && after < n && isAlphanumeric(text[after])) {
                            flushPlain(i)
                            appendInline(Array(text[(i + 1)..<close]), to: out, font: self.font(font, adding: .traitItalic), style: style)
                            i = after
                            plainStart = i
                            continue
                        }
                    }
                }
            }

            // Link.
            if c == 0x5B, let closeBracket = bracketEnd.next(in: text, from: i + 1),
               closeBracket + 1 < n, text[closeBracket + 1] == 0x28,
               let closeParen = parenEnd.next(in: text, from: closeBracket + 2) {
                flushPlain(i)
                let label = substring(i + 1, closeBracket)
                let destination = substring(closeBracket + 2, closeParen).trimmingCharacters(in: .whitespaces)
                var attributes: [NSAttributedString.Key: Any] = [
                    .font: font,
                    .foregroundColor: style.link,
                    .underlineStyle: NSUnderlineStyle.single.rawValue,
                ]
                if let url = linkURL(destination) {
                    attributes[.link] = url
                }
                out.append(NSAttributedString(string: label.isEmpty ? destination : label, attributes: attributes))
                i = closeParen + 1
                plainStart = i
                continue
            }

            i += 1
        }
        flushPlain(n)
    }

    /// Before iOS 17 a URL can't hold a space or a non-ASCII character unescaped, and
    /// `[notes](日本語 ノート.md)` should still be a link.
    private static func linkURL(_ destination: String) -> URL? {
        if let url = URL(string: destination) {
            return url
        }
        var allowed = CharacterSet.urlFragmentAllowed
        allowed.insert(charactersIn: "#%")
        return destination.addingPercentEncoding(withAllowedCharacters: allowed).flatMap(URL.init(string:))
    }

    /// nil if `operation` was cancelled along the way.
    static func render(_ markdown: String, style: WorkspaceMarkdownStyle, operation: WorkspaceFileOperation?) -> NSAttributedString? {
        let out = NSMutableAttributedString()
        guard !markdown.isEmpty else { return out }
        let baseFont = style.baseFont
        let baseSize = baseFont.pointSize
        let codeFont = UIFont.monospacedSystemFont(ofSize: baseSize - 1, weight: .regular)
        let inline = InlineStyle(color: style.text, codeFont: codeFont, codeColor: style.text, codeBackground: style.codeBackground, link: style.link)
        let quoteInline = InlineStyle(color: style.secondary, codeFont: codeFont, codeColor: style.text, codeBackground: style.codeBackground, link: style.link)

        let bodyStyle = NSMutableParagraphStyle()
        bodyStyle.paragraphSpacing = baseSize * 0.35
        let codeStyle = NSMutableParagraphStyle()
        codeStyle.firstLineHeadIndent = 8
        codeStyle.headIndent = 8
        codeStyle.paragraphSpacing = baseSize * 0.35
        let quoteStyle = NSMutableParagraphStyle()
        quoteStyle.firstLineHeadIndent = 12
        quoteStyle.headIndent = 12
        quoteStyle.paragraphSpacing = baseSize * 0.35

        let newline = NSAttributedString(string: "\n", attributes: [.font: baseFont, .foregroundColor: style.text])
        func styleParagraph(from start: Int, _ paragraph: NSParagraphStyle) {
            out.addAttribute(.paragraphStyle, value: paragraph, range: NSRange(location: start, length: out.length - start))
        }

        var codeLines: [String] = []
        func flushCode() {
            var attributes: [NSAttributedString.Key: Any] = [.font: codeFont, .foregroundColor: style.text, .paragraphStyle: codeStyle]
            if let background = style.codeBackground {
                attributes[.backgroundColor] = background
            }
            out.append(NSAttributedString(string: codeLines.joined(separator: "\n"), attributes: attributes))
            out.append(NSAttributedString(string: "\n", attributes: [.font: codeFont]))
            codeLines.removeAll()
        }

        let lines = markdown.replacingOccurrences(of: "\r\n", with: "\n").components(separatedBy: "\n")
        var inFence = false
        var lineIndex = 0
        while lineIndex < lines.count {
            if lineIndex & 0xFF == 0, operation?.isCancelled == true {
                return nil
            }
            let rawLine = lines[lineIndex]
            // From here `lineIndex` is the line after this one.
            lineIndex += 1
            let trimmed = rawLine.trimmingCharacters(in: .whitespaces)

            // Fenced code.
            if trimmed.hasPrefix("```") || trimmed.hasPrefix("~~~") {
                if inFence {
                    flushCode()
                }
                inFence.toggle()
                continue
            }
            if inFence {
                codeLines.append(rawLine)
                continue
            }

            if trimmed.isEmpty {
                out.append(newline)
                continue
            }

            // A pipe table: a header row followed by a separator row, as a monospaced grid.
            // Widths are in characters, so wide scripts and emoji can misalign slightly, which
            // buys tables with no layout machinery. Cells are verbatim, which is what keeps the
            // columns honest.
            if trimmed.contains("|"), lineIndex < lines.count, isTableSeparator(lines[lineIndex]) {
                var rows = [tableCells(trimmed)]
                lineIndex += 1
                while lineIndex < lines.count, lines[lineIndex].contains("|") {
                    rows.append(tableCells(lines[lineIndex]))
                    lineIndex += 1
                }
                let columnCount = rows.map(\.count).max() ?? 0
                var widths = [Int](repeating: 0, count: columnCount)
                for row in rows {
                    for (column, cell) in row.enumerated() {
                        widths[column] = max(widths[column], cell.count)
                    }
                }
                let aligned = widths.reduce(0, +) * rows.count <= tablePaddingLimit
                let headerFont = font(codeFont, adding: .traitBold)
                for (rowIndex, row) in rows.enumerated() {
                    var rendered = ""
                    for column in 0..<(aligned ? columnCount : row.count) {
                        let cell = column < row.count ? row[column] : ""
                        rendered += cell
                        if column + 1 < columnCount {
                            rendered += String(repeating: " ", count: aligned ? widths[column] - cell.count + 2 : 2)
                        }
                    }
                    rendered += "\n"
                    out.append(NSAttributedString(string: rendered, attributes: [
                        .font: rowIndex == 0 ? headerFont : codeFont,
                        .foregroundColor: style.text,
                        .paragraphStyle: codeStyle,
                    ]))
                }
                out.append(newline)
                continue
            }

            // Horizontal rule.
            if horizontalRules.contains(trimmed) {
                out.append(NSAttributedString(string: "————————\n", attributes: [.font: baseFont, .foregroundColor: style.secondary]))
                continue
            }

            // ATX heading.
            let trimmedBytes = trimmed.utf8
            let hashes = trimmedBytes.prefix { $0 == 0x23 }.count
            if (1...6).contains(hashes), trimmedBytes.dropFirst(hashes).first == 0x20 {
                let heading = String(decoding: trimmedBytes.dropFirst(hashes), as: UTF8.self).trimmingCharacters(in: .whitespaces)
                let scale: CGFloat = hashes == 1 ? 1.5 : (hashes == 2 ? 1.3 : (hashes == 3 ? 1.15 : 1.05))
                let headingFont = font(baseFont.withSize((baseSize * scale).rounded()), adding: .traitBold)
                let start = out.length
                appendInline(Array(heading.utf16), to: out, font: headingFont, style: inline)
                styleParagraph(from: start, bodyStyle)
                out.append(newline)
                continue
            }

            // Blockquote.
            if trimmed.hasPrefix(">") {
                let quote = String(trimmed.dropFirst()).trimmingCharacters(in: .whitespaces)
                let start = out.length
                appendInline(Array(quote.utf16), to: out, font: font(baseFont, adding: .traitItalic), style: quoteInline)
                styleParagraph(from: start, quoteStyle)
                out.append(newline)
                continue
            }

            // A bullet or numbered list item, nested by its leading spaces.
            let lineBytes = Array(rawLine.utf8)
            let lead = lineBytes.prefix { $0 == 0x20 }.count
            let body = lineBytes.dropFirst(lead)
            let first = body.first ?? 0
            let isBullet = (first == 0x2D || first == 0x2A || first == 0x2B) && body.dropFirst().first == 0x20
            let digits = body.prefix { $0 >= 0x30 && $0 <= 0x39 }.count
            let isNumbered = digits > 0 && body.dropFirst(digits).first == 0x2E && body.dropFirst(digits + 1).first == 0x20
            if isBullet || isNumbered {
                let marker = isBullet ? "•\t" : String(decoding: body.prefix(digits), as: UTF8.self) + ".\t"
                let item = String(decoding: body.dropFirst(isBullet ? 2 : digits + 2), as: UTF8.self).trimmingCharacters(in: .whitespaces)
                // Bounded, or a line of a million spaces indents past any window's width.
                let indent = (isBullet ? 16 : 22) + CGFloat(min(lead / 2, deepestListLevel)) * 14
                let paragraph = NSMutableParagraphStyle()
                paragraph.headIndent = indent
                paragraph.firstLineHeadIndent = indent - (isBullet ? 14 : 18)
                paragraph.paragraphSpacing = baseSize * 0.2
                paragraph.tabStops = [NSTextTab(textAlignment: .left, location: indent, options: [:])]
                let start = out.length
                out.append(NSAttributedString(string: marker, attributes: [
                    .font: isNumbered ? font(baseFont, adding: .traitBold) : baseFont,
                    .foregroundColor: style.text,
                ]))
                appendInline(Array(item.utf16), to: out, font: baseFont, style: inline)
                styleParagraph(from: start, paragraph)
                out.append(newline)
                continue
            }

            // A paragraph line.
            let start = out.length
            appendInline(Array(rawLine.utf16), to: out, font: baseFont, style: inline)
            styleParagraph(from: start, bodyStyle)
            out.append(newline)
        }
        // An unterminated fence.
        if inFence && !codeLines.isEmpty {
            flushCode()
        }
        return out
    }
}

/// A read-only Markdown reader: renders a file, follows relative links in place with a back
/// history, sends other kinds of file to their applets, and opens http and https links
/// externally when they're tapped.
@MainActor
final class WorkspaceMarkdownAppletViewController: WorkspaceAppletViewController, WorkspaceFileOpenable, WorkspaceStatefulTool, WorkspaceTextScalable, WorkspaceFocusable, UITextViewDelegate {
    private static let toolbarHeight: CGFloat = 44
    /// Real markdown that size is a pathology.
    private static let maxBytes = 4 * 1024 * 1024
    private static let renderQueue = DispatchQueue(label: "com.pscal.workspace.markdown", qos: .userInitiated)

    private static let markdownExtensions: Set<String> = ["", "md", "markdown"]
    private static let videoExtensions: Set<String> = ["mp4", "mov", "m4v", "avi", "mkv", "webm", "m3u", "m3u8"]
    private static let browserExtensions: Set<String> = ["html", "htm", "xhtml", "svg", "pdf"]
    private static let textExtensions: Set<String> = [
        "txt", "log", "conf", "cfg", "config", "json", "yaml", "yml",
        "xml", "csv", "tsv", "sh", "bash", "zsh", "c", "h", "cpp", "cc", "hpp", "m", "mm",
        "py", "rb", "js", "ts", "jsx", "tsx", "go", "rs", "java", "swift", "ini", "toml",
        "gitignore", "env", "plist", "properties", "gradle", "make", "mk", "cmake", "rst",
        "tex", "diff", "patch", "pas", "clike", "rea", "ae",
    ]

    private let toolbarView = UIView()
    private let backButton = UIButton(type: .system)
    private let titleLabel = UILabel()
    private let editButton = UIButton(type: .system)
    private let reloadButton = UIButton(type: .system)
    private let textView = UITextView()

    private(set) var currentPath: String?
    private var backHistory: [String] = []
    /// nil while a status message (loading, an error) is showing instead.
    private var rawMarkdown: String?
    private(set) var statusMessage: String?
    var renderedLengthForTesting: Int { rawMarkdown == nil ? 0 : textView.attributedText.length }
    /// Discards a stale read or render.
    private var loadGeneration = 0
    private var readOperation: WorkspaceFileOperation?
    private var renderOperation: WorkspaceFileOperation?
    private var renderedTextScale: CGFloat = 1

    deinit {
        readOperation?.cancel()
        renderOperation?.cancel()
    }

    override func viewDidLoad() {
        super.viewDidLoad()
        buildToolbar()
        buildTextView()
        NSLayoutConstraint.activate([
            toolbarView.topAnchor.constraint(equalTo: toolContentView.topAnchor),
            toolbarView.leadingAnchor.constraint(equalTo: toolContentView.leadingAnchor),
            toolbarView.trailingAnchor.constraint(equalTo: toolContentView.trailingAnchor),
            toolbarView.heightAnchor.constraint(equalToConstant: Self.toolbarHeight),
            textView.topAnchor.constraint(equalTo: toolbarView.bottomAnchor),
            textView.leadingAnchor.constraint(equalTo: toolContentView.leadingAnchor),
            textView.trailingAnchor.constraint(equalTo: toolContentView.trailingAnchor),
            textView.bottomAnchor.constraint(equalTo: toolContentView.bottomAnchor),
        ])
        statusMessage = "Open a Markdown file from the File Manager."
        updateContent()
    }

    private func buildToolbar() {
        toolbarView.translatesAutoresizingMaskIntoConstraints = false
        toolContentView.addSubview(toolbarView)

        configureIconButton(backButton, symbol: "chevron.left", label: "Back",
                            hint: "Goes back to the previous document", identifier: "workspace.markdown.back",
                            action: #selector(navigateBack))
        titleLabel.translatesAutoresizingMaskIntoConstraints = false
        titleLabel.font = .systemFont(ofSize: 13, weight: .medium)
        titleLabel.lineBreakMode = .byTruncatingTail
        titleLabel.textAlignment = .center
        titleLabel.text = "Markdown"
        titleLabel.accessibilityIdentifier = "workspace.markdown.title"
        titleLabel.setContentCompressionResistancePriority(.defaultLow, for: .horizontal)
        configureIconButton(editButton, symbol: "pencil", label: "Edit",
                            hint: "Opens this document in the text editor", identifier: "workspace.markdown.edit",
                            action: #selector(openInEditor))
        editButton.isHidden = !WorkspaceToolRegistry.isAvailable(WorkspaceToolRegistry.motePad)
        configureIconButton(reloadButton, symbol: "arrow.clockwise", label: "Reload",
                            hint: "Reads this document again", identifier: "workspace.markdown.reload",
                            action: #selector(reload))

        let stack = UIStackView(arrangedSubviews: [backButton, titleLabel, editButton, reloadButton])
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

    private func configureIconButton(_ button: UIButton, symbol: String, label: String, hint: String, identifier: String, action: Selector) {
        button.translatesAutoresizingMaskIntoConstraints = false
        button.setImage(UIImage(systemName: symbol), for: .normal)
        button.addTarget(self, action: action, for: .touchUpInside)
        button.widthAnchor.constraint(greaterThanOrEqualToConstant: 28).isActive = true
        button.accessibilityLabel = label
        button.accessibilityHint = hint
        button.accessibilityIdentifier = identifier
        button.isEnabled = false
    }

    private func buildTextView() {
        textView.translatesAutoresizingMaskIntoConstraints = false
        textView.isEditable = false
        textView.isSelectable = true
        textView.alwaysBounceVertical = true
        textView.delegate = self
        textView.backgroundColor = .clear
        textView.textContainerInset = UIEdgeInsets(top: 12, left: 12, bottom: 12, right: 12)
        // The renderer already makes the links. A second, independent pass would disagree
        // with its ranges.
        textView.dataDetectorTypes = []
        textView.accessibilityIdentifier = "workspace.markdown.text"
        toolContentView.addSubview(textView)
    }

    // MARK: - Theme and text size

    override func applyTheme() {
        super.applyTheme()
        let colors = theme
        // The theme's text colours are only guaranteed against its card surfaces, so the
        // content sits on a card rather than on the gradient.
        toolContentView.backgroundColor = colors.card.withAlphaComponent(0.95)
        titleLabel.textColor = colors.primary
        [backButton, editButton, reloadButton].forEach { $0.tintColor = colors.accent }
        textView.tintColor = colors.accent
        textView.linkTextAttributes = [
            .foregroundColor: colors.accent,
            .underlineStyle: NSUnderlineStyle.single.rawValue,
        ]
    }

    override func themeDidChange() {
        super.themeDidChange()
        // The colours are baked into the render.
        updateContent(keepingPosition: true)
    }

    /// Every font derives from the base font, so the whole document renders again, and the
    /// reading position is kept as a fraction of it.
    override func applyTextScale() {
        super.applyTextScale()
        guard abs(renderedTextScale - workspaceTextScale) >= 0.001 else { return }
        updateContent(keepingPosition: true)
    }

    // MARK: - Workspace

    func workspaceOpenFile(atPath path: String) {
        loadViewIfNeeded()
        backHistory.removeAll()
        load(path)
    }

    func workspaceToolStateForSaving() -> [String: Any]? {
        currentPath.map { ["path": $0] }
    }

    func workspaceRestoreToolState(_ state: [String: Any]) {
        // A file deleted since then shows the load error, honestly.
        if let path = state["path"] as? String, !path.isEmpty {
            workspaceOpenFile(atPath: path)
        }
    }

    /// Takes the keyboard from whatever terminal had it, as an applet with no typing does, and
    /// finds out if the file has gone while this window was behind another.
    func workspaceToolDidBecomeFrontmost() {
        if !textView.isFirstResponder {
            becomeFirstResponder()
        }
        guard let path = currentPath, rawMarkdown != nil else { return }
        let generation = loadGeneration
        WorkspaceFileService.shared.item(path) { [weak self] result in
            guard let self, self.loadGeneration == generation else { return }
            switch result {
            case .success(let item):
                if item.kind != .regular {
                    self.showStatus(self.message(for: WorkspaceFileError.system(ENOENT)))
                }
            case .failure(let error):
                self.showStatus(self.message(for: error))
            }
        }
    }

    override var canBecomeFirstResponder: Bool { true }

    // MARK: - Navigation

    @objc private func navigateBack() {
        guard let target = backHistory.popLast() else { return }
        load(target)
    }

    @objc private func reload() {
        if let currentPath {
            load(currentPath)
        }
    }

    @objc private func openInEditor() {
        if let currentPath {
            workspaceHost?.open(tool: WorkspaceToolRegistry.motePad, path: currentPath)
        }
    }

    /// By text alone, since the path is the shell's and not a real one: `..` stops at the root.
    private static func resolved(_ link: String, against directory: String) -> String {
        let joined = link.hasPrefix("/") ? link : directory + "/" + link
        var parts: [Substring] = []
        for part in joined.split(separator: "/") {
            if part == ".." {
                _ = parts.popLast()
            } else if part != "." {
                parts.append(part)
            }
        }
        return "/" + parts.joined(separator: "/")
    }

    private static func tool(forExtension fileExtension: String) -> String? {
        if WorkspaceImageViewerAppletViewController.supportedExtensions.contains(fileExtension) {
            return WorkspaceToolRegistry.imageViewer
        }
        if videoExtensions.contains(fileExtension) {
            return WorkspaceToolRegistry.videoPlayer
        }
        if browserExtensions.contains(fileExtension) {
            return WorkspaceToolRegistry.browser
        }
        if textExtensions.contains(fileExtension) {
            return WorkspaceToolRegistry.motePad
        }
        return nil
    }

    private func follow(_ url: URL) {
        if let scheme = url.scheme?.lowercased() {
            if scheme == "http" || scheme == "https" {
                UIApplication.shared.open(url)
            } else {
                showCantOpen(url.absoluteString)
            }
            return
        }
        // No scheme: a path, relative to this file unless it starts at the root. A #fragment
        // means nothing to the loader, and the URL may hold percent escapes the path doesn't.
        var target = url.relativeString
        if let fragment = target.firstIndex(of: "#") {
            target = String(target[..<fragment])
        }
        target = target.removingPercentEncoding ?? target
        // A link like "#section" has nowhere to go.
        guard !target.isEmpty, let currentPath else { return }
        let path = Self.resolved(target, against: WorkspaceFileService.parent(of: currentPath))
        let fileExtension = (target as NSString).pathExtension.lowercased()
        if Self.markdownExtensions.contains(fileExtension) {
            backHistory.append(currentPath)
            load(path)
        } else if let tool = Self.tool(forExtension: fileExtension), WorkspaceToolRegistry.isAvailable(tool) {
            workspaceHost?.open(tool: tool, path: path)
        } else {
            showCantOpen(target)
        }
    }

    private func showCantOpen(_ what: String) {
        let alert = UIAlertController(title: "Can’t Open", message: "“\(what)” can’t be opened from here.", preferredStyle: .alert)
        alert.addAction(UIAlertAction(title: "OK", style: .default))
        (workspaceHost ?? self).present(alert, animated: true)
    }

    /// Nothing opens unless it was tapped: a long press on a link offers no actions.
    func textView(_ textView: UITextView, shouldInteractWith url: URL, in characterRange: NSRange, interaction: UITextItemInteraction) -> Bool {
        if interaction == .invokeDefaultAction {
            follow(url)
        }
        return false
    }

    // MARK: - Loading

    private func load(_ path: String) {
        readOperation?.cancel()
        currentPath = path
        titleLabel.text = (path as NSString).lastPathComponent
        backButton.isEnabled = !backHistory.isEmpty
        editButton.isEnabled = true
        reloadButton.isEnabled = true
        showStatus("Loading…")

        loadGeneration += 1
        let generation = loadGeneration
        readOperation = WorkspaceFileService.shared.readFile(path, maxBytes: Self.maxBytes) { [weak self] result in
            guard let self, self.loadGeneration == generation else { return }
            self.readOperation = nil
            switch result {
            case .success(let data):
                self.decode(data, generation: generation)
            case .failure(let error):
                self.showStatus(self.message(for: error))
            }
        }
    }

    private func decode(_ data: Data, generation: Int) {
        Self.renderQueue.async { [weak self] in
            let markdown = WorkspaceMarkdownRenderer.decode(data)
            DispatchQueue.main.async { [weak self] in
                MainActor.assumeIsolated {
                    guard let self, self.loadGeneration == generation else { return }
                    if markdown.isEmpty {
                        // An empty window would look like one that failed to load.
                        self.showStatus("This file is empty.")
                    } else {
                        self.rawMarkdown = markdown
                        self.startRender(position: nil)
                    }
                }
            }
        }
    }

    private func message(for error: Error) -> String {
        if let fileError = error as? WorkspaceFileError {
            switch fileError {
            case .tooLarge:
                return "This file is larger than 4 MB and can’t be displayed here."
            case .system(ENOENT):
                return "“\(titleLabel.text ?? "This file")” isn’t there any more. It may have been deleted or moved."
            default:
                break
            }
        }
        return error.localizedDescription.isEmpty ? "Couldn’t open this file." : error.localizedDescription
    }

    private func showStatus(_ message: String) {
        renderOperation?.cancel()
        renderOperation = nil
        rawMarkdown = nil
        statusMessage = message
        updateContent()
    }

    /// Shows the status message if there is one, and renders the document if there is one. Both
    /// at once means the first render is under way: starting it again here means a theme or
    /// text size changed meanwhile, and it would come out in the old one.
    private func updateContent(keepingPosition: Bool = false) {
        renderedTextScale = workspaceTextScale
        if let statusMessage {
            textView.attributedText = NSAttributedString(string: statusMessage, attributes: [
                .font: UIFont.systemFont(ofSize: scaledFontSize(15)),
                .foregroundColor: theme.secondary,
            ])
        }
        if rawMarkdown != nil {
            startRender(position: keepingPosition ? readingPosition : nil)
        }
    }

    private func currentStyle() -> WorkspaceMarkdownStyle {
        let colors = theme
        let body = UIFont.preferredFont(forTextStyle: .body)
        return WorkspaceMarkdownStyle(baseFont: body.withSize(scaledFontSize(body.pointSize)),
                                      text: colors.primary,
                                      secondary: colors.secondary,
                                      codeBackground: colors.cardAlt.withAlphaComponent(0.5),
                                      link: colors.accent)
    }

    /// Renders off the main thread, and shows it unless something newer began.
    private func startRender(position: CGFloat?) {
        renderOperation?.cancel()
        guard let markdown = rawMarkdown else { return }
        let operation = WorkspaceFileOperation()
        renderOperation = operation
        let style = currentStyle()
        Self.renderQueue.async { [weak self] in
            let rendered = WorkspaceMarkdownRenderer.render(markdown, style: style, operation: operation)
            nonisolated(unsafe) let delivered = rendered
            DispatchQueue.main.async { [weak self] in
                MainActor.assumeIsolated {
                    guard let self, self.renderOperation === operation, let delivered else { return }
                    self.renderOperation = nil
                    self.statusMessage = nil
                    self.textView.attributedText = delivered
                    if let position {
                        self.restoreReadingPosition(position)
                    }
                }
            }
        }
    }

    private var readingPosition: CGFloat {
        let scrollable = textView.contentSize.height - textView.bounds.height
        return scrollable > 0 ? (textView.contentOffset.y + textView.adjustedContentInset.top) / scrollable : 0
    }

    private func restoreReadingPosition(_ position: CGFloat) {
        textView.layoutIfNeeded()
        let scrollable = textView.contentSize.height - textView.bounds.height
        if scrollable > 0 && position > 0 {
            textView.contentOffset = CGPoint(x: 0, y: min(1, position) * scrollable - textView.adjustedContentInset.top)
        }
    }
}
