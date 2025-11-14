import Foundation
import UIKit

private struct TerminalCell {
    var character: Character
    var attributes: TerminalAttributes

    static func blank(attributes: TerminalAttributes = TerminalAttributes()) -> TerminalCell {
        TerminalCell(character: " ", attributes: attributes)
    }
}

private struct TerminalAttributes: Equatable {
    var foreground: TerminalColor = .defaultForeground
    var background: TerminalColor = .defaultBackground
    var bold: Bool = false
    var underline: Bool = false
    var inverse: Bool = false

    mutating func reset() {
        self = TerminalAttributes()
    }
}

private enum TerminalColor: Equatable {
    case defaultForeground
    case defaultBackground
    case ansi(Int)
    case extended(Int)
    case rgb(Int, Int, Int)

    func resolvedUIColor(isForeground: Bool) -> UIColor {
        switch self {
        case .defaultForeground:
            return UIColor.label
        case .defaultBackground:
            return UIColor.systemBackground
        case .ansi(let index):
            let palette: [UIColor] = [
                UIColor(red: 0.0, green: 0.0, blue: 0.0, alpha: 1.0),
                UIColor(red: 0.86, green: 0.08, blue: 0.24, alpha: 1.0),
                UIColor(red: 0.0, green: 0.6, blue: 0.2, alpha: 1.0),
                UIColor(red: 0.97, green: 0.7, blue: 0.0, alpha: 1.0),
                UIColor(red: 0.2, green: 0.4, blue: 0.96, alpha: 1.0),
                UIColor(red: 0.55, green: 0.0, blue: 0.6, alpha: 1.0),
                UIColor(red: 0.0, green: 0.65, blue: 0.75, alpha: 1.0),
                UIColor(red: 0.93, green: 0.93, blue: 0.93, alpha: 1.0),
                UIColor(red: 0.3, green: 0.3, blue: 0.3, alpha: 1.0),
                UIColor(red: 1.0, green: 0.2, blue: 0.2, alpha: 1.0),
                UIColor(red: 0.13, green: 0.8, blue: 0.2, alpha: 1.0),
                UIColor(red: 1.0, green: 0.9, blue: 0.2, alpha: 1.0),
                UIColor(red: 0.35, green: 0.55, blue: 1.0, alpha: 1.0),
                UIColor(red: 0.88, green: 0.35, blue: 0.95, alpha: 1.0),
                UIColor(red: 0.0, green: 0.85, blue: 1.0, alpha: 1.0),
                UIColor(red: 1.0, green: 1.0, blue: 1.0, alpha: 1.0)
            ]
            return palette[max(0, min(index, palette.count - 1))]
        case .extended(let idx):
            return TerminalColor.colorForExtended(index: idx)
        case .rgb(let r, let g, let b):
            return UIColor(red: CGFloat(r) / 255.0, green: CGFloat(g) / 255.0, blue: CGFloat(b) / 255.0, alpha: 1.0)
        }
    }

    private static func colorForExtended(index: Int) -> UIColor {
        switch index {
        case 0...15:
            return TerminalColor.ansi(index).resolvedUIColor(isForeground: true)
        case 16...231:
            let idx = index - 16
            let r = idx / 36
            let g = (idx % 36) / 6
            let b = idx % 6
            func component(_ value: Int) -> Double {
                value == 0 ? 0 : Double(value * 40 + 55)
            }
            return UIColor(red: CGFloat(component(r)/255.0), green: CGFloat(component(g)/255.0), blue: CGFloat(component(b)/255.0), alpha: 1.0)
        case 232...255:
            let level = Double(index - 232) * 10.0 + 8.0
            let normalized = level / 255.0
            return UIColor(red: CGFloat(normalized), green: CGFloat(normalized), blue: CGFloat(normalized), alpha: 1.0)
        default:
            return UIColor.label
        }
    }
}

private struct TerminalFontCacheKey: Hashable {
    let pointSize: CGFloat
    let weightRawValue: CGFloat
}

private final class TerminalFontCache {
    private var cache: [TerminalFontCacheKey: UIFont] = [:]
    private let lock = NSLock()

    func font(pointSize: CGFloat, weight: UIFont.Weight) -> UIFont {
        let key = TerminalFontCacheKey(pointSize: pointSize, weightRawValue: weight.rawValue)
        lock.lock()
        defer { lock.unlock() }
        if let cached = cache[key] {
            return cached
        }
        let font = UIFont.monospacedSystemFont(ofSize: pointSize, weight: weight)
        cache[key] = font
        return font
    }

    func clear() {
        lock.lock()
        cache.removeAll()
        lock.unlock()
    }
}

final class TerminalBuffer {
    private enum ParserState {
        case normal
        case escape
        case csi
    }

    private var columns: Int
    private var rows: Int
    private let maxScrollback: Int

    private var grid: [[TerminalCell]]
    private var scrollback: [[TerminalCell]] = []
    private var cursorRow: Int = 0
    private var cursorCol: Int = 0
    private var savedCursor: (row: Int, col: Int)?
    private var cursorHidden: Bool = false
    private var currentAttributes = TerminalAttributes()

    private var parserState: ParserState = .normal
    private var csiParameters: [Int] = []
    private var currentParameter = ""
    private var csiPrivateMode = false

    private static let fontCache = TerminalFontCache()
    private static let fontCacheNotificationToken: NSObjectProtocol = {
        NotificationCenter.default.addObserver(forName: UIContentSizeCategory.didChangeNotification,
                                               object: nil,
                                               queue: nil) { _ in
            TerminalBuffer.fontCache.clear()
        }
    }()

private let syncQueue = DispatchQueue(label: "com.pscal.terminal.buffer", qos: .userInitiated)

struct TerminalSnapshot {
    fileprivate let lines: [[TerminalCell]]
}

    init(columns: Int = 80, rows: Int = 24, scrollback: Int = 500) {
        _ = TerminalBuffer.fontCacheNotificationToken
        self.columns = max(10, columns)
        self.rows = max(4, rows)
        self.maxScrollback = max(0, scrollback)
        self.grid = Array(repeating: TerminalBuffer.makeBlankRow(width: self.columns), count: self.rows)
    }

    @discardableResult
    func resize(columns newColumns: Int, rows newRows: Int) -> Bool {
        var didChange = false
        let clampedColumns = max(10, newColumns)
        let clampedRows = max(4, newRows)
        syncQueue.sync {
            var mutated = false
            if clampedColumns != columns {
                adjustColumnCount(to: clampedColumns)
                mutated = true
            }
            if clampedRows != rows {
                adjustRowCount(to: clampedRows)
                mutated = true
            }
            didChange = mutated
        }
        return didChange
    }

    func geometry() -> (columns: Int, rows: Int) {
        return syncQueue.sync { (columns, rows) }
    }

    func append(data: Data) {
        let decoded = String(decoding: data, as: UTF8.self)
        syncQueue.sync {
            for scalar in decoded.unicodeScalars {
                if scalar.value <= 0x7F {
                    process(byte: UInt8(scalar.value))
                } else {
                    insertCharacter(Character(scalar))
                }
            }
        }
    }

    func snapshot() -> TerminalSnapshot {
        let lines = syncQueue.sync {
            Array(scrollback.suffix(maxScrollback)) + grid
        }
        return TerminalSnapshot(lines: lines)
    }

    static func render(snapshot: TerminalSnapshot) -> [NSAttributedString] {
        var rendered = snapshot.lines.map { makeAttributedString(from: $0) }
        while rendered.count > 1, rendered.last?.string.isEmpty == true {
            rendered.removeLast()
        }
        if rendered.isEmpty {
            rendered = [NSAttributedString(string: "")]
        }
        return rendered
    }

    private func process(byte: UInt8) {
        switch parserState {
        case .normal:
            switch byte {
            case 0x0A: // LF
                newLine()
            case 0x0D: // CR
                cursorCol = 0
            case 0x08: // BS
                if cursorCol > 0 {
                    cursorCol -= 1
                }
            case 0x09: // TAB
                let nextStop = ((cursorCol / 8) + 1) * 8
                cursorCol = min(nextStop, columns - 1)
            case 0x1B: // ESC
                parserState = .escape
            case 0x07: // BEL
                break
            default:
                if byte >= 0x20 {
                    insertCharacter(Character(UnicodeScalar(byte)))
                }
            }
        case .escape:
            handleEscape(byte)
        case .csi:
            handleCSIByte(byte)
        }
    }

    private func handleEscape(_ byte: UInt8) {
        switch byte {
        case 0x5B: // '['
            parserState = .csi
            csiParameters.removeAll()
            currentParameter = ""
            csiPrivateMode = false
        case 0x37: // '7' save cursor
            savedCursor = (cursorRow, cursorCol)
            parserState = .normal
        case 0x38: // '8' restore cursor
            if let saved = savedCursor {
                cursorRow = saved.row
                cursorCol = saved.col
            }
            parserState = .normal
        default:
            parserState = .normal
        }
    }

    private func handleCSIByte(_ byte: UInt8) {
        switch byte {
        case 0x30...0x39:
            currentParameter.append(Character(UnicodeScalar(byte)))
        case 0x3B: // ';'
            flushCSIParameter()
        case 0x3F: // '?'
            csiPrivateMode = true
        default:
            if byte >= 0x40 && byte <= 0x7E {
                flushCSIParameter()
                handleCSICommand(byte)
                parserState = .normal
                csiParameters.removeAll()
                currentParameter = ""
                csiPrivateMode = false
            }
        }
    }

    private func flushCSIParameter() {
        if !currentParameter.isEmpty {
            csiParameters.append(Int(currentParameter) ?? 0)
            currentParameter = ""
        }
    }

    private func handleCSICommand(_ command: UInt8) {
        switch command {
        case 0x41: // A
            moveCursor(rowOffset: -(csiParameters.first ?? 1), colOffset: 0)
        case 0x42: // B
            moveCursor(rowOffset: csiParameters.first ?? 1, colOffset: 0)
        case 0x43: // C
            moveCursor(rowOffset: 0, colOffset: csiParameters.first ?? 1)
        case 0x44: // D
            moveCursor(rowOffset: 0, colOffset: -(csiParameters.first ?? 1))
        case 0x48, 0x66: // H, f
            let row = (csiParameters.first ?? 1) - 1
            let col = (csiParameters.dropFirst().first ?? 1) - 1
            cursorRow = clamp(row, lower: 0, upper: rows - 1)
            cursorCol = clamp(col, lower: 0, upper: columns - 1)
        case 0x4A: // J
            clearScreen(mode: csiParameters.first ?? 0)
        case 0x4B: // K
            clearLine(mode: csiParameters.first ?? 0)
        case 0x6D: // m
            applySGR()
        case 0x73: // s
            savedCursor = (cursorRow, cursorCol)
        case 0x75: // u
            if let saved = savedCursor {
                cursorRow = saved.row
                cursorCol = saved.col
            }
        case 0x68: // h
            if csiPrivateMode {
                handleDECPrivate(on: true)
            }
        case 0x6C: // l
            if csiPrivateMode {
                handleDECPrivate(on: false)
            }
        default:
            break
        }
    }

    private func handleDECPrivate(on: Bool) {
        guard let mode = csiParameters.first else { return }
        switch mode {
        case 25:
            cursorHidden = !on
            _ = cursorHidden
        default:
            break
        }
    }

    private func moveCursor(rowOffset: Int, colOffset: Int) {
        cursorRow = clamp(cursorRow + rowOffset, lower: 0, upper: rows - 1)
        cursorCol = clamp(cursorCol + colOffset, lower: 0, upper: columns - 1)
    }

    private func insertCharacter(_ character: Character) {
        grid[cursorRow][cursorCol] = TerminalCell(character: character, attributes: currentAttributes)
        cursorCol += 1
        if cursorCol >= columns {
            cursorCol = 0
            newLine()
        }
    }

    private func newLine() {
        cursorRow += 1
        if cursorRow >= rows {
            scrollUp()
            cursorRow = rows - 1
        }
        cursorCol = 0
    }

    private func scrollUp() {
        let firstLine = grid.removeFirst()
        scrollback.append(firstLine)
        if scrollback.count > maxScrollback {
            scrollback.removeFirst()
        }
        grid.append(makeBlankRow())
    }

    private func clearScreen(mode: Int) {
        switch mode {
        case 0:
            clearLine(mode: 0)
            if cursorRow + 1 < rows {
                for row in (cursorRow + 1)..<rows {
                    grid[row] = makeBlankRow()
                }
            }
        case 1:
            clearLine(mode: 1)
            if cursorRow > 0 {
                for row in 0..<cursorRow {
                    grid[row] = makeBlankRow()
                }
            }
        case 2, 3:
            grid = Array(repeating: makeBlankRow(), count: rows)
            scrollback.removeAll()
            cursorRow = 0
            cursorCol = 0
        default:
            break
        }
    }

    private func clearLine(mode: Int) {
        switch mode {
        case 0:
            if cursorCol < columns {
                for col in cursorCol..<columns {
                    grid[cursorRow][col] = TerminalCell.blank(attributes: currentAttributes)
                }
            }
        case 1:
            if cursorCol >= 0 {
                for col in 0...cursorCol {
                    grid[cursorRow][col] = TerminalCell.blank(attributes: currentAttributes)
                }
            }
        case 2:
            grid[cursorRow] = Array(repeating: TerminalCell.blank(attributes: currentAttributes), count: columns)
        default:
            break
        }
    }

    private func applySGR() {
        if csiParameters.isEmpty {
            currentAttributes.reset()
            return
        }
        var index = 0
        while index < csiParameters.count {
            let code = csiParameters[index]
            switch code {
            case 0:
                currentAttributes.reset()
            case 1:
                currentAttributes.bold = true
            case 4:
                currentAttributes.underline = true
            case 7:
                currentAttributes.inverse = true
            case 22:
                currentAttributes.bold = false
            case 24:
                currentAttributes.underline = false
            case 27:
                currentAttributes.inverse = false
            case 30...37:
                currentAttributes.foreground = .ansi(code - 30)
            case 39:
                currentAttributes.foreground = .defaultForeground
            case 40...47:
                currentAttributes.background = .ansi(code - 40)
            case 49:
                currentAttributes.background = .defaultBackground
            case 90...97:
                currentAttributes.foreground = .ansi(code - 90 + 8)
            case 100...107:
                currentAttributes.background = .ansi(code - 100 + 8)
            case 38:
                let (color, consumed) = parseExtendedColor(from: Array(csiParameters.suffix(from: index + 1)))
                if let color {
                    currentAttributes.foreground = color
                }
                index += consumed
            case 48:
                let (color, consumed) = parseExtendedColor(from: Array(csiParameters.suffix(from: index + 1)))
                if let color {
                    currentAttributes.background = color
                }
                index += consumed
            default:
                break
            }
            index += 1
        }
    }

    private func parseExtendedColor(from params: [Int]) -> (TerminalColor?, Int) {
        guard !params.isEmpty else { return (nil, 0) }
        let mode = params[0]
        switch mode {
        case 5:
            guard params.count >= 2 else { return (nil, 1) }
            return (.extended(params[1]), 1)
        case 2:
            guard params.count >= 4 else { return (nil, params.count - 1) }
            return (.rgb(params[1], params[2], params[3]), 3)
        default:
            return (nil, 0)
        }
    }

    private static func makeAttributedString(from row: [TerminalCell]) -> NSAttributedString {
        guard !row.isEmpty else {
            return NSAttributedString(string: "")
        }
        let mutable = NSMutableAttributedString()
        var currentAttributes = row.first!.attributes
        var buffer = ""

        func flush() {
            guard !buffer.isEmpty else { return }
            let attrs = nsAttributes(for: currentAttributes)
            let segment = NSAttributedString(string: buffer, attributes: attrs)
            mutable.append(segment)
            buffer.removeAll(keepingCapacity: true)
        }

        for cell in row {
            let displayCharacter: Character = {
                if cell.character.unicodeScalars.allSatisfy({ $0.value >= 0x20 }) {
                    return cell.character
                }
                return " "
            }()

            if cell.attributes == currentAttributes {
                buffer.append(displayCharacter)
            } else {
                flush()
                currentAttributes = cell.attributes
                buffer.append(displayCharacter)
            }
        }
        flush()
        return mutable.copy() as? NSAttributedString ?? NSAttributedString(string: "")
    }

    private static func nsAttributes(for attributes: TerminalAttributes) -> [NSAttributedString.Key: Any] {
        let colors = resolvedColors(attributes: attributes)
        let weight: UIFont.Weight = attributes.bold ? .bold : .regular
        let font = cachedFont(for: weight)
        var result: [NSAttributedString.Key: Any] = [
            .foregroundColor: colors.foreground,
            .backgroundColor: colors.background,
            .font: font
        ]
        if attributes.underline {
            result[.underlineStyle] = NSUnderlineStyle.single.rawValue
            result[.underlineColor] = colors.foreground
        }
        return result
    }

    private static func cachedFont(for weight: UIFont.Weight) -> UIFont {
        let pointSize = UIFont.preferredFont(forTextStyle: .body).pointSize
        return fontCache.font(pointSize: pointSize, weight: weight)
    }

    private static func resolvedColors(attributes: TerminalAttributes) -> (foreground: UIColor, background: UIColor) {
        if attributes.inverse {
            return (
                attributes.background.resolvedUIColor(isForeground: false),
                attributes.foreground.resolvedUIColor(isForeground: true)
            )
        } else {
            return (
                attributes.foreground.resolvedUIColor(isForeground: true),
                attributes.background.resolvedUIColor(isForeground: false)
            )
        }
    }

    private func clamp(_ value: Int, lower: Int, upper: Int) -> Int {
        return max(lower, min(value, upper))
    }

    private func adjustColumnCount(to newColumns: Int) {
        func resizeRow(_ row: inout [TerminalCell]) {
            if newColumns > row.count {
                row.append(contentsOf: Array(repeating: blankCell(), count: newColumns - row.count))
            } else if newColumns < row.count {
                row = Array(row.prefix(newColumns))
            }
        }

        for index in scrollback.indices {
            resizeRow(&scrollback[index])
        }
        for index in grid.indices {
            resizeRow(&grid[index])
        }
        columns = newColumns
        cursorCol = clamp(cursorCol, lower: 0, upper: max(newColumns - 1, 0))
        if var saved = savedCursor {
            saved.col = clamp(saved.col, lower: 0, upper: max(newColumns - 1, 0))
            savedCursor = saved
        }
    }

    private func adjustRowCount(to newRows: Int) {
        if newRows > rows {
            let blankRow = makeBlankRow()
            for _ in 0..<(newRows - rows) {
                grid.append(blankRow)
            }
        } else if newRows < rows {
            let trimCount = rows - newRows
            if trimCount > 0 {
                let removed = grid.prefix(trimCount)
                scrollback.append(contentsOf: removed)
                if scrollback.count > maxScrollback {
                    let overflow = scrollback.count - maxScrollback
                    if overflow > 0 && overflow <= scrollback.count {
                        scrollback.removeFirst(overflow)
                    }
                }
                grid.removeFirst(min(trimCount, grid.count))
                cursorRow = clamp(cursorRow - trimCount, lower: 0, upper: max(newRows - 1, 0))
                if var saved = savedCursor {
                    saved.row = clamp(saved.row - trimCount, lower: 0, upper: max(newRows - 1, 0))
                    savedCursor = saved
                }
            }
        }
        rows = newRows
    }

    private static func blankCell(with attributes: TerminalAttributes = TerminalAttributes()) -> TerminalCell {
        TerminalCell.blank(attributes: attributes)
    }

    private static func makeBlankRow(width: Int, attributes: TerminalAttributes = TerminalAttributes()) -> [TerminalCell] {
        Array(repeating: TerminalCell.blank(attributes: attributes), count: width)
    }

    private func blankCell(attributes: TerminalAttributes? = nil) -> TerminalCell {
        TerminalBuffer.blankCell(with: attributes ?? currentAttributes)
    }

    private func makeBlankRow(width: Int? = nil, attributes: TerminalAttributes? = nil) -> [TerminalCell] {
        return TerminalBuffer.makeBlankRow(width: width ?? columns, attributes: attributes ?? currentAttributes)
    }
}
