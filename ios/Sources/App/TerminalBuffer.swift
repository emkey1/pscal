import Foundation
import UIKit

// MARK: - Helper Structures

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
            return TerminalFontSettings.shared.foregroundColor
        case .defaultBackground:
            return TerminalFontSettings.shared.backgroundColor
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

// MARK: - Main Buffer Class

final class TerminalBuffer {
    
    private enum ParserState {
        case normal
        case escape
        case csi
        case osc
    }
    
    enum MouseMode {
        case none
        case click
        case drag
    }
    
    enum MouseEncoding {
        case normal
        case utf8
        case sgr
    }
    
    // MARK: - Properties
    private(set) var columns: Int
    private(set) var rows: Int
    private let maxScrollback: Int
    private let dsrResponder: ((Data) -> Void)?
    private var resizeRequestHandler: ((Int, Int) -> Void)?
    private var resetHandler: (() -> Void)?
    
    private var grid: [[TerminalCell]]
    private var scrollback: [[TerminalCell]] = []
    private var scrollRegionTop: Int
    private var scrollRegionBottom: Int
    
    private var cursorRow: Int = 0
    private var cursorCol: Int = 0
    private var savedCursor: (row: Int, col: Int)?
    private var cursorHidden: Bool = false
    private var wrapPending: Bool = false
    private var lastPrintedChar: Character? = nil
    
    private var tabStops: Set<Int> = []
    
    private var currentAttributes = TerminalAttributes()
    private var originModeEnabled: Bool = false
    private var suppressNextRemoteLF: Bool = false
    private var utf8ContinuationBytes: Int = 0
    private var utf8Codepoint: UInt32 = 0
    private var utf8ReplayByte: UInt8? = nil
    
    private(set) var bracketedPasteEnabled: Bool = false
    private(set) var insertMode: Bool = false
    private(set) var autoWrapMode: Bool = true
    
    private(set) var mouseMode: MouseMode = .none
    private(set) var mouseEncoding: MouseEncoding = .normal
    
    var onMouseModeChange: ((MouseMode, MouseEncoding) -> Void)?
    
    private var parserState: ParserState = .normal
    private var csiParameters: [Int] = []
    private var currentParameter = ""
    private var csiPrivateMode = false
    private let syncQueue = DispatchQueue(label: "com.pscal.terminal.buffer", qos: .userInitiated)
    private var usingAlternateScreen: Bool = false
    private struct ScreenState {
        var grid: [[TerminalCell]]
        var scrollback: [[TerminalCell]]
        var cursorRow: Int
        var cursorCol: Int
        var savedCursor: (row: Int, col: Int)?
        var tabStops: Set<Int>
        var currentAttributes: TerminalAttributes
        var originModeEnabled: Bool
        var wrapPending: Bool
        var insertMode: Bool
        var autoWrapMode: Bool
        var cursorHidden: Bool
        var scrollRegionTop: Int
        var scrollRegionBottom: Int
        var columns: Int
        var rows: Int
    }
    private var savedPrimaryScreen: ScreenState?
    
    private static let fontCache = TerminalFontCache()
    private static let fontCacheNotificationToken: NSObjectProtocol = {
        NotificationCenter.default.addObserver(forName: UIContentSizeCategory.didChangeNotification,
                                               object: nil,
                                               queue: nil) { _ in
            TerminalBuffer.fontCache.clear()
        }
    }()
    private let inputQueue = DispatchQueue(label: "com.pscal.terminal.input", attributes: .concurrent)
    private var bufferedInput: [UInt8] = []
    private var pendingCompletions: [() -> Void] = []
    private var drainScheduled = false
    
    struct TerminalSnapshot {
        struct Cursor {
            let row: Int
            let col: Int
        }
        
        fileprivate let lines: [[TerminalCell]]
        let cursor: Cursor?
        let defaultBackground: UIColor
        let visibleRows: Int
        
        var lineCount: Int {
            return lines.count
        }
        
        func clampedRow(_ row: Int) -> Int {
            guard !lines.isEmpty else { return 0 }
            return max(0, min(row, lines.count - 1))
        }
        
        func clampedColumn(row: Int, column: Int) -> Int {
            guard row >= 0 && row < lines.count else { return 0 }
            return max(0, min(column, lines[row].count))
        }
        
        func utf16Offset(row: Int, column: Int) -> Int {
            guard row >= 0 && row < lines.count else { return 0 }
            return TerminalBuffer.utf16Offset(in: lines[row], column: column)
        }
    }
    
    init(columns: Int,
         rows: Int,
         scrollback: Int = 500,
         dsrResponder: ((Data) -> Void)? = nil,
         resizeHandler: ((Int, Int) -> Void)? = nil) {
        self.dsrResponder = dsrResponder
        self.resizeRequestHandler = resizeHandler
        self.resetHandler = nil
        _ = TerminalBuffer.fontCacheNotificationToken
        self.columns = max(10, columns)
        self.rows = max(4, rows)
        self.maxScrollback = max(0, scrollback)
        self.grid = Array(repeating: TerminalBuffer.makeBlankRow(width: self.columns), count: self.rows)
        self.scrollRegionTop = 0
        self.scrollRegionBottom = self.rows - 1
        
        resetTabStops()
    }
    
    func setResizeHandler(_ handler: ((Int, Int) -> Void)?) {
        resizeRequestHandler = handler
    }
    
    func setResetHandler(_ handler: (() -> Void)?) {
        resetHandler = handler
    }
    
    @discardableResult
    func resize(columns newColumns: Int, rows newRows: Int) -> Bool {
        var didChange = false
        let clampedColumns = max(10, newColumns)
        let clampedRows = max(4, newRows)
        syncQueue.sync {
            let wasFullScrollRegion = (scrollRegionTop == 0 && scrollRegionBottom == rows - 1)
            var mutated = false
            if clampedColumns != columns {
                adjustColumnCount(to: clampedColumns)
                mutated = true
            }
            if clampedRows != rows {
                adjustRowCount(to: clampedRows)
                mutated = true
            }
            if mutated {
                if wasFullScrollRegion {
                    scrollRegionTop = 0
                    scrollRegionBottom = clampedRows - 1
                }
                clampScrollRegionBounds()
            }
            didChange = mutated
        }
        return didChange
    }
    
    func geometry() -> (columns: Int, rows: Int) {
        return syncQueue.sync { (columns, rows) }
    }
    
    func append(data: Data, onProcessed: (() -> Void)? = nil) {
        inputQueue.async(flags: .barrier) {
            self.bufferedInput.append(contentsOf: data)
            if let onProcessed {
                self.pendingCompletions.append(onProcessed)
            }
            if !self.drainScheduled {
                self.drainScheduled = true
                self.syncQueue.async {
                    self.drainInputBufferLocked()
                }
            }
        }
    }

    // No-op placeholder retained for future filtering if needed.
    
    private func drainInputBufferLocked() {
        while true {
            var pending: [UInt8] = []
            var completions: [() -> Void] = []
            inputQueue.sync(flags: .barrier) {
                pending = bufferedInput
                bufferedInput.removeAll()
                completions = pendingCompletions
                pendingCompletions.removeAll()
            }
            guard !pending.isEmpty else {
                for completion in completions {
                    DispatchQueue.main.async { completion() }
                }
                break
            }
            for byte in pending {
                process(byte: byte)
            }
            for completion in completions {
                DispatchQueue.main.async { completion() }
            }
        }
        var needsAnotherDrain = false
        inputQueue.sync(flags: .barrier) {
            if bufferedInput.isEmpty {
                drainScheduled = false
            } else {
                needsAnotherDrain = true
            }
        }
        if needsAnotherDrain {
            syncQueue.async {
                self.drainInputBufferLocked()
            }
        }
    }
    
    func consumeInput(count: Int) -> [UInt8] {
        var result: [UInt8] = []
        inputQueue.sync(flags: .barrier) {
            guard count > 0 else { return }
            let slice = bufferedInput.prefix(count)
            result.append(contentsOf: slice)
            bufferedInput.removeFirst(result.count)
        }
        return result
    }
    
    func snapshot(includeScrollback: Bool = false) -> TerminalSnapshot {
        return syncQueue.sync {
            let retainedScrollback = Array(scrollback.suffix(maxScrollback))
            let combinedLines = retainedScrollback + grid
            let totalLines = combinedLines.count
            var lines = combinedLines
            var visibleRows = totalLines
            var cursorRowIndex: Int? = nil

            if includeScrollback {
                cursorRowIndex = retainedScrollback.count + cursorRow
            } else {
                let visibleStart = max(0, totalLines - rows)
                let visibleEnd = min(totalLines, visibleStart + rows)
                var trimmedLines = Array(combinedLines[visibleStart..<visibleEnd])
                let deficit = rows - trimmedLines.count
                var paddingCount = 0
                if deficit > 0 {
                    paddingCount = deficit
                    let blankRow = TerminalBuffer.makeBlankRow(width: columns)
                    let paddingLines = Array(repeating: blankRow, count: paddingCount)
                    trimmedLines = paddingLines + trimmedLines
                }
                lines = trimmedLines
                visibleRows = trimmedLines.count

                let absoluteCursorRow = retainedScrollback.count + cursorRow
                cursorRowIndex = absoluteCursorRow - visibleStart + paddingCount
            }

            let cursorInfo: TerminalSnapshot.Cursor?
            if cursorHidden {
                cursorInfo = nil
            } else if let rowIndex = cursorRowIndex,
                      rowIndex >= 0 && rowIndex < lines.count {
                let clampedCol = clamp(cursorCol, lower: 0, upper: columns - 1)
                cursorInfo = TerminalSnapshot.Cursor(row: rowIndex, col: clampedCol)
            } else {
                cursorInfo = nil
            }
            
            let referenceRow = grid.last ?? grid.first
            let referenceAttributes = referenceRow?.first?.attributes ?? TerminalAttributes()
            let backgroundColor = TerminalBuffer.resolvedColors(attributes: referenceAttributes).background
            
            return TerminalSnapshot(lines: lines,
                                    cursor: cursorInfo,
                                    defaultBackground: backgroundColor,
                                    visibleRows: visibleRows)
        }
    }
    
    func reset() {
        var handler: (() -> Void)?
        syncQueue.sync {
            currentAttributes.reset()
            scrollback.removeAll()
            grid = Array(repeating: makeBlankRow(), count: rows)
            cursorRow = 0
            cursorCol = 0
            savedCursor = nil
            cursorHidden = false
            originModeEnabled = false
            suppressNextRemoteLF = false
            parserState = .normal
            csiParameters.removeAll()
            currentParameter = ""
            csiPrivateMode = false
            scrollRegionTop = 0
            scrollRegionBottom = rows - 1
            bracketedPasteEnabled = false
            insertMode = false
            autoWrapMode = true
            mouseMode = .none
            mouseEncoding = .normal
            wrapPending = false
            lastPrintedChar = nil
            resetTabStops()
            handler = resetHandler
        }
        notifyMouseChange()
        handler?()
    }
    
    func echoUserInput(_ text: String) {
        guard !text.isEmpty else { return }
        syncQueue.sync {
            for scalar in text.unicodeScalars {
                handleEchoScalar(scalar)
            }
        }
    }
    
    static func render(snapshot: TerminalSnapshot) -> [NSAttributedString] {
        var rendered = snapshot.lines.map { makeAttributedString(from: $0) }
        if rendered.isEmpty {
            rendered = [NSAttributedString(string: "")]
        }
        let desiredRows = max(1, snapshot.visibleRows)
        if rendered.count > desiredRows {
            rendered = Array(rendered.suffix(desiredRows))
        }
        return rendered
    }
    
    private func process(byte: UInt8, fromEcho: Bool = false) {
            if !fromEcho && byte != 0x0A && byte != 0x0D {
                suppressNextRemoteLF = false
            }
            switch parserState {
            case .normal:
                switch byte {
                case 0x0A: // LF
                    wrapPending = false
                    resetUTF8Decoder()
                    if !fromEcho && suppressNextRemoteLF {
                        suppressNextRemoteLF = false
                        break
                    }
                    newLine(resetColumn: true)
                    if fromEcho {
                        suppressNextRemoteLF = true
                    }
                case 0x0D: // CR
                    wrapPending = false
                    resetUTF8Decoder()
                    cursorCol = 0
                case 0x08: // BS
                    wrapPending = false
                    resetUTF8Decoder()
                    if cursorCol > 0 {
                        cursorCol -= 1
                    }
                case 0x09: // TAB
                    wrapPending = false
                    resetUTF8Decoder()
                    
                    var nextStop = columns - 1
                    for stop in tabStops.sorted() {
                        if stop > cursorCol {
                            nextStop = stop
                            break
                        }
                    }
                    cursorCol = min(nextStop, columns - 1)
                    
                case 0x1B: // ESC
                    resetUTF8Decoder()
                    parserState = .escape
                case 0x07: // BEL
                    resetUTF8Decoder()
                    break
                default:
                    if byte >= 0x20 {
                        if let scalar = decodeUTF8Byte(byte) {
                            insertCharacter(Character(scalar))
                            if let replay = utf8ReplayByte {
                                utf8ReplayByte = nil
                                if let replayScalar = decodeUTF8Byte(replay) {
                                    insertCharacter(Character(replayScalar))
                                }
                            }
                        }
                    }
                }
            case .escape:
                handleEscape(byte)
            case .csi:
                handleCSIByte(byte)
            case .osc:
                if byte == 0x07 {
                    parserState = .normal
                } else if byte == 0x1B {
                    parserState = .normal
                }
            }
        }

        private func resetUTF8Decoder() {
            utf8ContinuationBytes = 0
            utf8Codepoint = 0
            utf8ReplayByte = nil
        }

        private func decodeUTF8Byte(_ byte: UInt8) -> UnicodeScalar? {
            if utf8ContinuationBytes == 0 {
                switch byte {
                case 0x00...0x7F:
                    return UnicodeScalar(byte)
                case 0xC2...0xDF:
                    utf8ContinuationBytes = 1
                    utf8Codepoint = UInt32(byte & 0x1F)
                    return nil
                case 0xE0...0xEF:
                    utf8ContinuationBytes = 2
                    utf8Codepoint = UInt32(byte & 0x0F)
                    return nil
                case 0xF0...0xF4:
                    utf8ContinuationBytes = 3
                    utf8Codepoint = UInt32(byte & 0x07)
                    return nil
                default:
                    resetUTF8Decoder()
                    return UnicodeScalar(0xFFFD)
                }
            } else {
                if (byte & 0xC0) != 0x80 {
                    resetUTF8Decoder()
                    utf8ReplayByte = byte
                    return UnicodeScalar(0xFFFD)
                }
                utf8Codepoint = (utf8Codepoint << 6) | UInt32(byte & 0x3F)
                utf8ContinuationBytes -= 1
                if utf8ContinuationBytes == 0 {
                    let value = utf8Codepoint
                    resetUTF8Decoder()
                    if let scalar = UnicodeScalar(value) {
                        return scalar
                    }
                    return UnicodeScalar(0xFFFD)
                }
                return nil
            }
        }

        private func handleEscape(_ byte: UInt8) {
            switch byte {
            case 0x5B: // '['
                parserState = .csi
                csiParameters.removeAll()
                currentParameter = ""
                csiPrivateMode = false
            case 0x5D: // ']'
                parserState = .osc
            case 0x44: // 'D' index
                newLine(resetColumn: false)
                parserState = .normal
            case 0x45: // 'E' next line
                newLine(resetColumn: true)
                parserState = .normal
            case 0x48: // 'H' HTS
                tabStops.insert(cursorCol)
                parserState = .normal
            case 0x37: // '7' save cursor
                savedCursor = (cursorRow, cursorCol)
                parserState = .normal
            case 0x38: // '8' restore cursor
                if let saved = savedCursor {
                    cursorRow = saved.row
                    cursorCol = saved.col
                }
                parserState = .normal
            case 0x4D: // 'M' reverse index
                reverseIndex()
                parserState = .normal
            case 0x63: // 'c' full reset
                resetState()
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
            case 0x40: // @
                insertBlankCharacters(max(1, csiParameters.first ?? 1))
            case 0x41: // A
                moveCursor(rowOffset: -(csiParameters.first ?? 1), colOffset: 0)
            case 0x42: // B
                moveCursor(rowOffset: csiParameters.first ?? 1, colOffset: 0)
            case 0x43: // C
                moveCursor(rowOffset: 0, colOffset: csiParameters.first ?? 1)
            case 0x44: // D
                moveCursor(rowOffset: 0, colOffset: -(csiParameters.first ?? 1))
            case 0x45: // E
                let amount = max(1, csiParameters.first ?? 1)
                cursorRow = clampRow(cursorRow + amount)
                cursorCol = 0
            case 0x46: // F
                let amount = max(1, csiParameters.first ?? 1)
                cursorRow = clampRow(cursorRow - amount)
                cursorCol = 0
            case 0x47: // G
                let column = clamp((csiParameters.first ?? 1) - 1, lower: 0, upper: columns - 1)
                cursorCol = column
            case 0x48, 0x66: // H, f
                cursorRow = resolvedRow(from: csiParameters.first)
                cursorCol = clamp((csiParameters.dropFirst().first ?? 1) - 1, lower: 0, upper: columns - 1)
            case 0x5A: // Z
                let count = max(1, csiParameters.first ?? 1)
                for _ in 0..<count {
                    var prevStop = 0
                    let sorted = tabStops.sorted()
                    for stop in sorted.reversed() {
                        if stop < cursorCol {
                            prevStop = stop
                            break
                        }
                    }
                    cursorCol = prevStop
                }
            case 0x49: // I
                let count = max(1, csiParameters.first ?? 1)
                for _ in 0..<count {
                    var nextStop = columns - 1
                    for stop in tabStops.sorted() {
                        if stop > cursorCol {
                            nextStop = stop
                            break
                        }
                    }
                    cursorCol = min(nextStop, columns - 1)
                }
            case 0x67: // g
                let mode = csiParameters.first ?? 0
                if mode == 0 {
                    tabStops.remove(cursorCol)
                } else if mode == 3 {
                    tabStops.removeAll()
                }
            case 0x62: // b
                let count = max(1, csiParameters.first ?? 1)
                if let last = lastPrintedChar {
                    for _ in 0..<count {
                        insertCharacter(last)
                    }
                }
            case 0x4A: // J
                clearScreen(mode: csiParameters.first ?? 0)
            case 0x4B: // K
                clearLine(mode: csiParameters.first ?? 0)
            case 0x4C: // L
                insertLines(max(1, csiParameters.first ?? 1))
            case 0x4D: // M
                deleteLines(max(1, csiParameters.first ?? 1))
            case 0x50: // P
                deleteCharacters(max(1, csiParameters.first ?? 1))
            case 0x53: // S
                scrollRegionUp(by: max(1, csiParameters.first ?? 1), trackScrollback: isFullScrollRegion)
            case 0x54: // T
                scrollRegionDown(by: max(1, csiParameters.first ?? 1))
            case 0x58: // X
                eraseCharacters(max(1, csiParameters.first ?? 1))
            case 0x6D: // m
                applySGR()
            case 0x6E: // n (DSR request)
                handleDSRRequest()
            case 0x72: // r (set scroll region)
                setScrollRegion(topParameter: csiParameters.first,
                                bottomParameter: csiParameters.dropFirst().first)
            case 0x61: // a (HPR)
                moveCursor(rowOffset: 0, colOffset: csiParameters.first ?? 1)
            case 0x65: // e (VPR)
                moveCursor(rowOffset: csiParameters.first ?? 1, colOffset: 0)
            case 0x64: // d (VPA)
                cursorRow = resolvedRow(from: csiParameters.first)
            case 0x63: // c (DA)
                handleDeviceAttributes()
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
                } else {
                    handleANSIMode(on: true)
                }
            case 0x6C: // l
                if csiPrivateMode {
                    handleDECPrivate(on: false)
                } else {
                    handleANSIMode(on: false)
                }
                
            case 0x74: // t (window ops)
                handleWindowCommand()
            default:
                break
            }
        }

        private func handleDECPrivate(on: Bool) {
            guard let mode = csiParameters.first else { return }
            switch mode {
            case 6:
                originModeEnabled = on
                cursorRow = originModeEnabled ? scrollRegionTop : 0
                cursorCol = 0
            case 7:
                autoWrapMode = on
            case 25:
                cursorHidden = !on
            case 2004:
                bracketedPasteEnabled = on
            case 1000:
                mouseMode = on ? .click : .none
                notifyMouseChange()
            case 1002:
                mouseMode = on ? .drag : .none
                notifyMouseChange()
            case 1006:
                mouseEncoding = on ? .sgr : .normal
                notifyMouseChange()
            case 1049:
                if on {
                    enterAlternateScreen()
                } else {
                    exitAlternateScreen()
                }
            default:
                break
            }
        }
        
        private func handleANSIMode(on: Bool) {
            guard let mode = csiParameters.first else { return }
            switch mode {
            case 4: // IRM
                insertMode = on
            default:
                break
            }
        }
        
        private func notifyMouseChange() {
            let mode = mouseMode
            let enc = mouseEncoding
            DispatchQueue.main.async { [weak self] in
                self?.onMouseModeChange?(mode, enc)
            }
        }

        private func captureScreenState() -> ScreenState {
            return ScreenState(
                grid: grid,
                scrollback: scrollback,
                cursorRow: cursorRow,
                cursorCol: cursorCol,
                savedCursor: savedCursor,
                tabStops: tabStops,
                currentAttributes: currentAttributes,
                originModeEnabled: originModeEnabled,
                wrapPending: wrapPending,
                insertMode: insertMode,
                autoWrapMode: autoWrapMode,
                cursorHidden: cursorHidden,
                scrollRegionTop: scrollRegionTop,
                scrollRegionBottom: scrollRegionBottom,
                columns: columns,
                rows: rows
            )
        }

        private func applyScreenState(_ state: ScreenState, targetColumns: Int, targetRows: Int) {
            grid = state.grid
            scrollback = state.scrollback
            cursorRow = state.cursorRow
            cursorCol = state.cursorCol
            savedCursor = state.savedCursor
            tabStops = state.tabStops
            currentAttributes = state.currentAttributes
            originModeEnabled = state.originModeEnabled
            wrapPending = state.wrapPending
            insertMode = state.insertMode
            autoWrapMode = state.autoWrapMode
            cursorHidden = state.cursorHidden
            scrollRegionTop = state.scrollRegionTop
            scrollRegionBottom = state.scrollRegionBottom
            columns = state.columns
            rows = state.rows
            clampScrollRegionBounds()
            if columns != targetColumns {
                adjustColumnCount(to: targetColumns)
            }
            if rows != targetRows {
                adjustRowCount(to: targetRows)
            }
            clampScrollRegionBounds()
        }

        private func enterAlternateScreen() {
            if usingAlternateScreen {
                return
            }
            savedPrimaryScreen = captureScreenState()
            usingAlternateScreen = true
            scrollback.removeAll()
            grid = Array(repeating: makeBlankRow(), count: rows)
            cursorRow = 0
            cursorCol = 0
            savedCursor = nil
            resetTabStops()
            wrapPending = false
            lastPrintedChar = nil
            scrollRegionTop = 0
            scrollRegionBottom = rows - 1
        }

        private func exitAlternateScreen() {
            guard usingAlternateScreen else { return }
            usingAlternateScreen = false
            if let state = savedPrimaryScreen {
                let targetColumns = columns
                let targetRows = rows
                applyScreenState(state, targetColumns: targetColumns, targetRows: targetRows)
            } else {
                // No saved state; just clear.
                grid = Array(repeating: makeBlankRow(), count: rows)
                scrollback.removeAll()
                cursorRow = 0
                cursorCol = 0
                savedCursor = nil
                resetTabStops()
                wrapPending = false
                scrollRegionTop = 0
                scrollRegionBottom = rows - 1
            }
            savedPrimaryScreen = nil
        }

        private func handleWindowCommand() {
            guard let command = csiParameters.first else { return }
            switch command {
            case 8:
                if csiParameters.count >= 3 {
                    let rows = csiParameters[1]
                    let columns = csiParameters[2]
                    resizeRequestHandler?(columns, rows)
                }
            default:
                break
            }
        }

        private func moveCursor(rowOffset: Int, colOffset: Int) {
            wrapPending = false
            cursorRow = clampRow(cursorRow + rowOffset)
            cursorCol = clamp(cursorCol + colOffset, lower: 0, upper: columns - 1)
        }

        private func handleDSRRequest() {
            guard let request = csiParameters.first else { return }
            switch request {
            case 5:
                sendTerminalResponse("\u{001B}[0n")
            case 6:
                let row = cursorRow + 1
                let col = cursorCol + 1
                sendTerminalResponse("\u{001B}[\(row);\(col)R")
            default:
                // Suppress responses for unsupported DSRs to avoid echoing noise into
                // application output when raw mode isn't fully honored by the bridge.
                break
            }
        }

        private func sendTerminalResponse(_ string: String) {
            guard let responder = dsrResponder, let data = string.data(using: .utf8) else {
                return
            }
            responder(data)
        }

        private func insertCharacter(_ character: Character) {
            if wrapPending {
                wrapPending = false
                if autoWrapMode {
                    newLine(resetColumn: true)
                } else {
                    cursorCol = columns - 1
                }
            }

            if cursorRow >= rows { cursorRow = rows - 1 }
            if cursorCol >= columns { cursorCol = columns - 1 }

            let cell = TerminalCell(character: character, attributes: currentAttributes)
            lastPrintedChar = character

            if insertMode {
                var line = grid[cursorRow]
                if cursorCol < line.count {
                    line.insert(cell, at: cursorCol)
                    line.removeLast()
                    grid[cursorRow] = line
                }
            } else {
                grid[cursorRow][cursorCol] = cell
            }

            cursorCol += 1
            
            if cursorCol >= columns {
                wrapPending = true
                cursorCol = columns - 1
            }
        }

        private func newLine(resetColumn: Bool) {
            let wasWithinRegion = cursorRow >= scrollRegionTop && cursorRow <= scrollRegionBottom
            cursorRow += 1
            if wasWithinRegion && cursorRow > scrollRegionBottom {
                scrollRegionUp(by: 1, trackScrollback: isFullScrollRegion)
                cursorRow = scrollRegionBottom
            } else if cursorRow >= rows {
                cursorRow = rows - 1
            }
            if resetColumn {
                cursorCol = 0
            } else {
                cursorCol = clamp(cursorCol, lower: 0, upper: columns - 1)
            }
        }

        private func normalizeGridForEraseOperations() {
            let safeRows = max(rows, 1)
            let safeColumns = max(columns, 1)

            if grid.count < safeRows {
                let missing = safeRows - grid.count
                for _ in 0..<missing {
                    grid.append(makeBlankRow(width: safeColumns))
                }
            } else if grid.count > safeRows {
                grid.removeLast(grid.count - safeRows)
            }

            for index in grid.indices {
                if grid[index].count < safeColumns {
                    grid[index].append(contentsOf: Array(repeating: blankCell(), count: safeColumns - grid[index].count))
                } else if grid[index].count > safeColumns {
                    grid[index].removeLast(grid[index].count - safeColumns)
                }
            }

            if grid.isEmpty {
                grid = Array(repeating: makeBlankRow(width: safeColumns), count: safeRows)
            }

            cursorRow = clamp(cursorRow, lower: 0, upper: max(grid.count - 1, 0))
            cursorCol = clamp(cursorCol, lower: 0, upper: max(safeColumns - 1, 0))
        }

        private func clearScreen(mode: Int) {
            normalizeGridForEraseOperations()
            wrapPending = false
            switch mode {
            case 0:
                clearLine(mode: 0)
                if cursorRow + 1 < grid.count {
                    for row in (cursorRow + 1)..<grid.count {
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
                grid = Array(repeating: makeBlankRow(), count: max(rows, 1))
                scrollback.removeAll()
                cursorRow = 0
                cursorCol = 0
            default:
                break
            }
        }

        private func clearLine(mode: Int) {
            normalizeGridForEraseOperations()
            guard !grid.isEmpty else { return }
            cursorRow = clamp(cursorRow, lower: 0, upper: max(grid.count - 1, 0))
            let lineWidth = grid[cursorRow].count
            guard lineWidth > 0 else { return }
            cursorCol = clamp(cursorCol, lower: 0, upper: max(lineWidth - 1, 0))
            switch mode {
            case 0:
                if cursorCol < lineWidth {
                    for col in cursorCol..<lineWidth {
                        grid[cursorRow][col] = TerminalCell.blank(attributes: currentAttributes)
                    }
                }
            case 1:
                for col in 0...cursorCol {
                    grid[cursorRow][col] = TerminalCell.blank(attributes: currentAttributes)
                }
            case 2:
                grid[cursorRow] = Array(repeating: TerminalCell.blank(attributes: currentAttributes), count: lineWidth)
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
                return (.extended(params[1]), 2)
            case 2:
                guard params.count >= 4 else { return (nil, params.count - 1) }
                return (.rgb(params[1], params[2], params[3]), 4)
            default:
                return (nil, 1)
            }
        }

        private func clampRow(_ value: Int) -> Int {
            if originModeEnabled {
                return clamp(value, lower: scrollRegionTop, upper: scrollRegionBottom)
            }
            return clamp(value, lower: 0, upper: rows - 1)
        }

        private func resolvedRow(from parameter: Int?) -> Int {
            let base = clamp((parameter ?? 1) - 1, lower: 0, upper: rows - 1)
            if originModeEnabled {
                let relativeMax = max(scrollRegionBottom - scrollRegionTop, 0)
                let relative = clamp(base, lower: 0, upper: relativeMax)
                return scrollRegionTop + relative
            }
            return base
        }

        private func handleDeviceAttributes() {
            let response = "\u{001B}[?1;0c"
            sendTerminalResponse(response)
        }

        private func reverseIndex() {
            if cursorRow > scrollRegionTop && cursorRow <= scrollRegionBottom {
                cursorRow -= 1
            } else if cursorRow == scrollRegionTop {
                scrollRegionDown(by: 1)
            } else {
                cursorRow = max(cursorRow - 1, 0)
            }
        }

        private func setScrollRegion(topParameter: Int?, bottomParameter: Int?) {
            wrapPending = false
            let maxRowIndex = max(rows - 1, 0)
            let resolvedTop = clamp((topParameter ?? 1) - 1, lower: 0, upper: maxRowIndex)
            var resolvedBottom = clamp((bottomParameter ?? rows) - 1, lower: 0, upper: maxRowIndex)
            if resolvedBottom < resolvedTop {
                resolvedBottom = resolvedTop
            }
            scrollRegionTop = resolvedTop
            scrollRegionBottom = resolvedBottom
            if originModeEnabled {
                cursorRow = scrollRegionTop
            } else {
                cursorRow = 0
            }
            cursorCol = 0
        }

        private func clampScrollRegionBounds() {
            let maxRowIndex = max(rows - 1, 0)
            scrollRegionTop = clamp(scrollRegionTop, lower: 0, upper: maxRowIndex)
            scrollRegionBottom = clamp(scrollRegionBottom, lower: scrollRegionTop, upper: maxRowIndex)
            cursorRow = clampRow(cursorRow)
            wrapPending = false
        }

        private var isFullScrollRegion: Bool {
            return scrollRegionTop == 0 && scrollRegionBottom == rows - 1
        }

        private func scrollRegionUp(by count: Int, trackScrollback: Bool = false) {
            guard scrollRegionTop <= scrollRegionBottom else { return }
            let height = scrollRegionBottom - scrollRegionTop + 1
            let amount = clamp(count, lower: 1, upper: height)
            for _ in 0..<amount {
                let removed = grid.remove(at: scrollRegionTop)
                if trackScrollback {
                    appendScrollbackLine(removed)
                }
                grid.insert(makeBlankRow(), at: scrollRegionBottom)
            }
        }

        private func scrollRegionDown(by count: Int) {
            guard scrollRegionTop <= scrollRegionBottom else { return }
            let height = scrollRegionBottom - scrollRegionTop + 1
            let amount = clamp(count, lower: 1, upper: height)
            for _ in 0..<amount {
                _ = grid.remove(at: scrollRegionBottom)
                grid.insert(makeBlankRow(), at: scrollRegionTop)
            }
        }

        private func insertLines(_ count: Int) {
            guard cursorRow >= scrollRegionTop && cursorRow <= scrollRegionBottom else { return }
            let available = scrollRegionBottom - cursorRow + 1
            let amount = clamp(count, lower: 1, upper: available)
            for _ in 0..<amount {
                grid.insert(makeBlankRow(), at: cursorRow)
                grid.remove(at: scrollRegionBottom + 1)
            }
        }

        private func deleteLines(_ count: Int) {
            guard cursorRow >= scrollRegionTop && cursorRow <= scrollRegionBottom else { return }
            let available = scrollRegionBottom - cursorRow + 1
            let amount = clamp(count, lower: 1, upper: available)
            for _ in 0..<amount {
                grid.remove(at: cursorRow)
                grid.insert(makeBlankRow(), at: scrollRegionBottom)
            }
        }

        private func insertBlankCharacters(_ count: Int) {
            guard cursorRow >= 0 && cursorRow < grid.count else { return }
            guard cursorCol >= 0 && cursorCol < columns else { return }
            let available = columns - cursorCol
            guard available > 0 else { return }
            let amount = min(max(1, count), available)
            var line = grid[cursorRow]
            for _ in 0..<amount {
                line.insert(blankCell(), at: cursorCol)
                line.removeLast()
            }
            grid[cursorRow] = line
        }

        private func deleteCharacters(_ count: Int) {
            guard cursorRow >= 0 && cursorRow < grid.count else { return }
            guard cursorCol >= 0 && cursorCol < columns else { return }
            let available = columns - cursorCol
            guard available > 0 else { return }
            let amount = min(max(1, count), available)
            var line = grid[cursorRow]
            line.removeSubrange(cursorCol..<(cursorCol + amount))
            line.append(contentsOf: Array(repeating: blankCell(), count: amount))
            grid[cursorRow] = line
        }

        private func eraseCharacters(_ count: Int) {
            guard cursorRow >= 0 && cursorRow < grid.count else { return }
            guard cursorCol >= 0 && cursorCol < columns else { return }
            guard count > 0 else { return }
            let end = min(columns, cursorCol + count)
            guard end > cursorCol else { return }
            for index in cursorCol..<end {
                grid[cursorRow][index] = blankCell()
            }
        }

        private func appendScrollbackLine(_ line: [TerminalCell]) {
            scrollback.append(line)
            if scrollback.count > maxScrollback {
                let overflow = scrollback.count - maxScrollback
                if overflow > 0 {
                    scrollback.removeFirst(overflow)
                }
            }
        }

        private func resetState() {
            currentAttributes.reset()
            scrollback.removeAll()
            grid = Array(repeating: makeBlankRow(), count: rows)
            cursorRow = 0
            cursorCol = 0
            savedCursor = nil
            cursorHidden = false
            originModeEnabled = false
            suppressNextRemoteLF = false
            parserState = .normal
            csiParameters.removeAll()
            currentParameter = ""
            csiPrivateMode = false
            scrollRegionTop = 0
            scrollRegionBottom = rows - 1
            resetHandler?()
            bracketedPasteEnabled = false
            insertMode = false
            autoWrapMode = true
            mouseMode = .none
            mouseEncoding = .normal
            wrapPending = false
            lastPrintedChar = nil
            resetTabStops()
            notifyMouseChange()
        }
        
        private func resetTabStops() {
            tabStops.removeAll()
            for i in stride(from: 8, to: 2000, by: 8) {
                tabStops.insert(i)
            }
        }

        fileprivate static func utf16Offset(in row: [TerminalCell], column: Int) -> Int {
            guard !row.isEmpty else { return 0 }
            let clamped = max(0, min(column, row.count))
            var offset = 0
            if clamped == 0 {
                return 0
            }
            for idx in 0..<clamped {
                let scalarCount = String(row[idx].character).utf16.count
                offset += scalarCount
            }
            return offset
        }

        fileprivate static func makeAttributedString(from row: [TerminalCell]) -> NSAttributedString {
            guard !row.isEmpty else {
                return NSAttributedString(string: "")
            }
            if row.allSatisfy({ $0.character == " " }) {
                return NSAttributedString(string: "")
            }

            let mutable = NSMutableAttributedString()
            var currentAttributes = row.first!.attributes
            var buffer = ""
            buffer.reserveCapacity(row.count)

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
            let pointSize = TerminalFontSettings.shared.pointSize
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
            wrapPending = false // Reset
            resetTabStops() // Re-calculate tabs if needed (usually stays at 8)
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

        private func handleEchoScalar(_ scalar: UnicodeScalar) {
            let value = scalar.value
            switch value {
            case 0x1B:
                parserState = .normal
                return
            case 0x0A, 0x0D, 0x09:
                resetUTF8Decoder()
                currentAttributes = attributesAtCursor()
                process(byte: UInt8(value), fromEcho: true)
            case 0x08:
                resetUTF8Decoder()
                process(byte: 0x08, fromEcho: true)
            case 0x7F:
                resetUTF8Decoder()
                applyBackspaceErase()
            default:
                guard value >= 0x20 else { return }
                currentAttributes = attributesAtCursor()
                let utf8Bytes = Array(String(scalar).utf8)
                for byte in utf8Bytes {
                    process(byte: byte, fromEcho: true)
                }
            }
        }

        private func attributesAtCursor() -> TerminalAttributes {
            if cursorRow >= 0 && cursorRow < grid.count &&
                cursorCol >= 0 && cursorCol < columns {
                return grid[cursorRow][cursorCol].attributes
            }
            if let lastRow = grid.last, let first = lastRow.first {
                return first.attributes
            }
            return currentAttributes
        }

        private func applyBackspaceErase() {
            guard cursorCol > 0 else { return }
            process(byte: 0x08, fromEcho: true)
            if cursorRow >= 0 && cursorRow < grid.count &&
                cursorCol >= 0 && cursorCol < columns {
                let attrs = grid[cursorRow][cursorCol].attributes
                grid[cursorRow][cursorCol] = TerminalCell.blank(attributes: attrs)
            }
        }
    }
