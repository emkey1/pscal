import Combine
import Foundation
import Darwin
import UIKit

private let runtimeLogMirrorsToConsole: Bool = {
    guard let value = ProcessInfo.processInfo.environment["PSCALI_RUNTIME_STDERR"] else {
        return false
    }
    return value != "0"
}()

func runtimeDebugLog(_ message: String) {
    appendRuntimeDebugLog(message)
}

@_cdecl("pscalRuntimeDebugLog")
func pscalRuntimeDebugLogBridge(_ message: UnsafePointer<CChar>?) {
    guard let message else { return }
    appendRuntimeDebugLog(String(cString: message))
}

private final class RuntimeLogger {
    static let runtime = RuntimeLogger(filename: "pscal_runtime.log")

    private let queue: DispatchQueue
    private let fileURL: URL
    private static let formatter: ISO8601DateFormatter = {
        let formatter = ISO8601DateFormatter()
        formatter.formatOptions = [.withInternetDateTime, .withFractionalSeconds]
        return formatter
    }()

    private static func logsDirectoryURL() -> URL {
        let documents = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask).first
            ?? URL(fileURLWithPath: NSTemporaryDirectory(), isDirectory: true)
        return documents
            .appendingPathComponent("var", isDirectory: true)
            .appendingPathComponent("log", isDirectory: true)
    }

    private init(filename: String) {
        self.queue = DispatchQueue(label: "com.pscal.runtime.log.\(filename)", qos: .utility)
        self.fileURL = RuntimeLogger.logsDirectoryURL().appendingPathComponent(filename, isDirectory: false)
    }

    func append(_ message: String) {
        let timestamp = RuntimeLogger.formatter.string(from: Date())
        let line = "[\(timestamp)] \(message)\n"
        guard let data = line.data(using: .utf8) else { return }
        queue.async {
            let directory = self.fileURL.deletingLastPathComponent()
            let fileManager = FileManager.default
            if !fileManager.fileExists(atPath: directory.path) {
                try? fileManager.createDirectory(at: directory, withIntermediateDirectories: true)
            }
            if fileManager.fileExists(atPath: self.fileURL.path) {
                do {
                    let handle = try FileHandle(forWritingTo: self.fileURL)
                    defer { try? handle.close() }
                    do {
                        try handle.seekToEnd()
                    } catch {
                        // ignore seek errors, fall through to write (which appends at current position)
                    }
                    handle.write(data)
                } catch {
                    try? data.write(to: self.fileURL, options: .atomic)
                }
            } else {
                try? data.write(to: self.fileURL, options: .atomic)
            }
        }
    }
}

private func appendRuntimeDebugLog(_ message: String) {
    guard !message.isEmpty else { return }
    RuntimeLogger.runtime.append(message)
    if runtimeLogMirrorsToConsole {
        NSLog("%@", message)
    }
}

final class PscalRuntimeBootstrap: ObservableObject {
    static let shared = PscalRuntimeBootstrap()

    @Published private(set) var screenText: NSAttributedString = NSAttributedString(string: "Launching exsh...")
    @Published private(set) var exitStatus: Int32?
    @Published private(set) var cursorInfo: TerminalCursorInfo?
    @Published private(set) var terminalBackgroundColor: UIColor = UIColor.systemBackground
    @Published private(set) var elvisRenderToken: UInt64 = 0

    private var started = false
    private let stateQueue = DispatchQueue(label: "com.pscal.runtime.state")
    private let launchQueue = DispatchQueue(label: "com.pscal.runtime.launch", qos: .userInitiated)
    // Dedicated pthread stack for the runtime (libdispatch workers are ~512KB).
    private let runtimeStackSizeBytes = 4 * 1024 * 1024
    private var handlerContext: UnsafeMutableRawPointer?
    private let terminalBuffer: TerminalBuffer
    private enum GeometrySource: Hashable {
        case main
        case elvis
    }
    private let inputQueue = DispatchQueue(label: "com.pscal.runtime.input", qos: .userInitiated)
    private var geometryBySource: [GeometrySource: TerminalGeometryMetrics]
    private var activeGeometry: TerminalGeometryMetrics
    private var activeGeometrySource: GeometrySource = .main
    private var elvisModeActive: Bool = false
    private var elvisRefreshPending: Bool = false
    private var appearanceObserver: NSObjectProtocol?
    private lazy var outputHandler: PSCALRuntimeOutputHandler = { data, length, context in
        guard let context, let base = data else { return }
        let bootstrap = Unmanaged<PscalRuntimeBootstrap>.fromOpaque(context).takeUnretainedValue()
        bootstrap.consumeOutput(buffer: base, length: Int(length))
    }

    private lazy var exitHandler: PSCALRuntimeExitHandler = { status, context in
        guard let context else { return }
        let bootstrap = Unmanaged<PscalRuntimeBootstrap>.fromOpaque(context).takeUnretainedValue()
        bootstrap.handleExit(status: status)
    }

    private init() {
        let initialMetrics = PscalRuntimeBootstrap.defaultGeometryMetrics()
        runtimeDebugLog("[Geometry] bootstrap size columns=\(initialMetrics.columns) rows=\(initialMetrics.rows)")
        self.geometryBySource = [.main: initialMetrics, .elvis: initialMetrics]
        self.activeGeometry = initialMetrics
        self.terminalBuffer = TerminalBuffer(columns: initialMetrics.columns,
                                             rows: initialMetrics.rows,
                                             scrollback: 400,
                                             dsrResponder: { data in
            data.withUnsafeBytes { buffer in
                guard let base = buffer.baseAddress else { return }
                let pointer = base.assumingMemoryBound(to: CChar.self)
                PSCALRuntimeSendInput(pointer, buffer.count)
            }
        })
        self.terminalBuffer.setResizeHandler { [weak self] columns, rows in
            self?.handleTerminalResizeRequest(columns: columns, rows: rows)
        }
        PSCALRuntimeUpdateWindowSize(Int32(initialMetrics.columns), Int32(initialMetrics.rows))
        appearanceObserver = NotificationCenter.default.addObserver(forName: TerminalFontSettings.appearanceDidChangeNotification,
                                                                     object: nil,
                                                                     queue: .main) { [weak self] _ in
            self?.scheduleRender()
        }
    }

    func sendPasted(_ text: String) {
        guard !text.isEmpty else { return }
        let wrapped: String
        if terminalBuffer.bracketedPasteEnabled {
            wrapped = "\u{1B}[200~" + text + "\u{1B}[201~"
        } else {
            wrapped = text
        }
        send(wrapped)
    }

    func start() {
        let shouldStart = stateQueue.sync { () -> Bool in
            guard !started else { return false }
            started = true
            return true
        }
        guard shouldStart else { return }

        RuntimeAssetInstaller.shared.prepareWorkspace()
        if let runnerPath = RuntimeAssetInstaller.shared.ensureToolRunnerExecutable() {
            setenv("PSCALI_TOOL_RUNNER_PATH", runnerPath, 1)
        }

        terminalBuffer.reset()
        DispatchQueue.main.async {
            self.screenText = NSAttributedString(string: "Launching exsh...")
            self.exitStatus = nil
            self.terminalBackgroundColor = UIColor.systemBackground
            self.cursorInfo = nil
        }

        handlerContext = Unmanaged.passUnretained(self).toOpaque()
        PSCALRuntimeConfigureHandlers(outputHandler, exitHandler, handlerContext)

        launchQueue.async {
            let args = ["exsh"]
            var cArgs: [UnsafeMutablePointer<CChar>?] = args.map { strdup($0) }
            let argc = Int32(cArgs.count)
            cArgs.append(nil)
            defer {
                cArgs.forEach { if let ptr = $0 { free(ptr) } }
            }
            cArgs.withUnsafeMutableBufferPointer { buffer in
                let stackSize: size_t = numericCast(self.runtimeStackSizeBytes)
                _ = PSCALRuntimeLaunchExshWithStackSize(argc, buffer.baseAddress, stackSize)
            }
        }
    }

    func send(_ text: String) {
        guard let data = text.data(using: .utf8), !data.isEmpty else { return }
        if ElvisTerminalBridge.shared.interceptInputIfNeeded(data: data) {
            return
        }
        echoLocallyIfNeeded(text)
        let bytes = [UInt8](data) // copy for async safety
        inputQueue.async {
            let chunkSize = 512
            var offset = 0
            while offset < bytes.count {
                let len = min(chunkSize, bytes.count - offset)
                bytes.withUnsafeBytes { raw in
                    guard let base = raw.baseAddress?.advanced(by: offset) else { return }
                    PSCALRuntimeSendInput(base.assumingMemoryBound(to: CChar.self), len)
                }
                offset += len
                if offset < bytes.count {
                    usleep(2000) // allow the PTY to drain
                }
            }
        }
    }

    func updateTerminalSize(columns: Int, rows: Int) {
        runtimeDebugLog("[Geometry] main update request columns=\(columns) rows=\(rows)")
        updateGeometry(from: .main, columns: columns, rows: rows)
    }

    func updateElvisWindowSize(columns: Int, rows: Int) {
        runtimeDebugLog("[Geometry] elvis update request columns=\(columns) rows=\(rows)")
        updateGeometry(from: .elvis, columns: columns, rows: rows)
    }

    func resetTerminalState() {
        let cols = terminalBuffer.columns
        let rows = terminalBuffer.rows
        if cols > 0 && rows > 0 {
            PSCALRuntimeUpdateWindowSize(Int32(cols), Int32(rows))
        }
        terminalBuffer.reset()
        DispatchQueue.main.async {
            self.screenText = NSAttributedString(string: "")
            self.cursorInfo = nil
        }
        runtimeDebugLog("[Runtime] terminal reset invoked")
        send("\u{1B}c") // RIS
    }

    func currentScreenText(maxLength: Int = 8000) -> String {
        let text = screenText.string
        let utf8 = text.utf8
        if utf8.count <= maxLength {
            return text
        }
        let dropCount = utf8.count - maxLength
        if let idx = text.utf8.index(text.utf8.startIndex, offsetBy: dropCount, limitedBy: text.utf8.endIndex),
           let scalarIndex = String.UTF8View.Index(idx, within: text) {
            return String(text[scalarIndex...])
        }
        return text.suffix(maxLength).description
    }

    @objc
    public func consumeOutput(buffer: UnsafePointer<Int8>, length: Int) {
        guard length > 0 else { return }
        let dataCopy = Data(bytes: buffer, count: length)
        free(UnsafeMutableRawPointer(mutating: buffer))
        terminalBuffer.append(data: dataCopy)
        scheduleRender()
    }

    private func handleExit(status: Int32) {
        runtimeDebugLog("[Runtime] exsh exit detected (status=\(status)); scheduling restart")
        let stackSymbols = Thread.callStackSymbols.joined(separator: "\n")
        RuntimeLogger.runtime.append("Call stack for exit status \(status):\n\(stackSymbols)\n")
        stateQueue.async {
            self.started = false
        }
        DispatchQueue.main.async {
            self.exitStatus = status
            let restartDelay: TimeInterval = 0.1
            runtimeDebugLog("[Runtime] restarting exsh in \(restartDelay)s after status \(status)")
            DispatchQueue.main.asyncAfter(deadline: .now() + restartDelay) {
                runtimeDebugLog("[Runtime] relaunching exsh after prior exit status \(status)")
                self.start()
            }
        }
    }

    private func handleTerminalResizeRequest(columns: Int, rows: Int) {
        runtimeDebugLog("[Geometry] remote resize request columns=\(columns) rows=\(rows); reasserting active \(activeGeometry.columns)x\(activeGeometry.rows)")
        DispatchQueue.main.async {
            self.refreshActiveGeometry(forceRuntimeUpdate: true)
        }
    }

    private func scheduleRender(preserveBackground: Bool = false) {
        if isElvisModeActive() {
            refreshElvisDisplay()
            return
        }
        let snapshot = terminalBuffer.snapshot()
        let renderResult = PscalRuntimeBootstrap.renderJoined(snapshot: snapshot)
        let backgroundColor = snapshot.defaultBackground
        DispatchQueue.main.async {
            self.screenText = renderResult.text
            self.cursorInfo = renderResult.cursor
            if !preserveBackground {
                self.terminalBackgroundColor = backgroundColor
            }
        }
    }

    func isElvisModeActive() -> Bool {
        return stateQueue.sync { elvisModeActive }
    }

    func setElvisModeActive(_ active: Bool) {
        stateQueue.sync {
            elvisModeActive = active
        }
        refreshActiveGeometry(forceRuntimeUpdate: true)
        if active {
            refreshElvisDisplay()
        } else {
            scheduleRender()
        }
    }

    func refreshElvisDisplay() {
        guard ElvisTerminalBridge.shared.isActive else { return }
        let shouldSchedule: Bool = stateQueue.sync {
            if elvisRefreshPending {
                return false
            }
            elvisRefreshPending = true
            return true
        }
        guard shouldSchedule else { return }
        DispatchQueue.main.async {
            self.terminalBackgroundColor = TerminalFontSettings.shared.backgroundColor
            self.elvisRenderToken &+= 1
            ElvisWindowManager.shared.refreshWindow()
            self.stateQueue.sync {
                self.elvisRefreshPending = false
            }
        }
    }


    private func shouldEchoLocally() -> Bool {
        return PSCALRuntimeIsVirtualTTY() != 0
    }

    private func echoLocallyIfNeeded(_ text: String) {
        guard shouldEchoLocally(), shouldEchoText(text) else { return }
        terminalBuffer.echoUserInput(text)
        scheduleRender(preserveBackground: true)
    }

    private func shouldEchoText(_ text: String) -> Bool {
        for scalar in text.unicodeScalars {
            if scalar.value == 0x1B {
                return false
            }
        }
        return true
    }

    private func updateGeometry(from source: GeometrySource, columns: Int, rows: Int) {
        let clampedColumns = max(10, columns)
        let clampedRows = max(4, rows)
        let metrics = TerminalGeometryMetrics(columns: clampedColumns, rows: clampedRows)
        geometryBySource[source] = metrics
        refreshActiveGeometry()
    }

    private func refreshActiveGeometry(forceRuntimeUpdate: Bool = false) {
        let desiredSource = determineDesiredGeometrySource()
        let desiredMetrics = geometryBySource[desiredSource] ?? geometryBySource[.main] ?? activeGeometry
        let geometryChanged = desiredMetrics != activeGeometry || desiredSource != activeGeometrySource
        if geometryChanged {
            runtimeDebugLog("[Geometry] switching to \(desiredSource) columns=\(desiredMetrics.columns) rows=\(desiredMetrics.rows)")
            activeGeometry = desiredMetrics
            activeGeometrySource = desiredSource
            let shouldResize = (desiredSource == .main)
            applyActiveGeometry(resizeTerminalBuffer: shouldResize, forceRuntimeUpdate: true)
        } else if forceRuntimeUpdate {
            runtimeDebugLog("[Geometry] refreshing existing geometry source=\(desiredSource) columns=\(desiredMetrics.columns) rows=\(desiredMetrics.rows)")
            applyActiveGeometry(resizeTerminalBuffer: false, forceRuntimeUpdate: true)
        } else {
            activeGeometrySource = desiredSource
        }
    }

    private func determineDesiredGeometrySource() -> GeometrySource {
        if isElvisModeActive() && ElvisWindowManager.shared.isVisible {
            return .elvis
        }
        return .main
    }

    private func applyActiveGeometry(resizeTerminalBuffer: Bool, forceRuntimeUpdate: Bool) {
        let metrics = activeGeometry
        if resizeTerminalBuffer {
            let resized = terminalBuffer.resize(columns: metrics.columns, rows: metrics.rows)
            if resized {
                scheduleRender()
            }
        }
        if forceRuntimeUpdate || resizeTerminalBuffer {
            runtimeDebugLog("[Geometry] applying runtime window size columns=\(metrics.columns) rows=\(metrics.rows) resizeBuffer=\(resizeTerminalBuffer) force=\(forceRuntimeUpdate)")
            PSCALRuntimeUpdateWindowSize(Int32(metrics.columns), Int32(metrics.rows))
        }
    }

    private static func defaultGeometryMetrics() -> TerminalGeometryMetrics {
        let font = TerminalFontSettings.shared.currentFont
        let screenSize = UIScreen.main.bounds.size
        let charWidth = max(1.0, ("W" as NSString).size(withAttributes: [.font: font]).width)
        let lineHeight = max(1.0, font.lineHeight)
        let columns = max(10, Int(floor(screenSize.width / charWidth)))
        let rows = max(4, Int(floor(screenSize.height / lineHeight)))
        return TerminalGeometryMetrics(columns: columns, rows: rows)
    }

    private static func renderJoined(snapshot: TerminalBuffer.TerminalSnapshot) -> (text: NSAttributedString, cursor: TerminalCursorInfo?) {
        let renderedLines = TerminalBuffer.render(snapshot: snapshot)
        let result = NSMutableAttributedString()
        let cursorRow = snapshot.cursor?.row
        let cursorCol = snapshot.cursor?.col ?? 0
        let rawLineCount = snapshot.lineCount
        var resolvedCursorRow: Int?
        if let row = cursorRow, rawLineCount > 0 {
            resolvedCursorRow = min(max(row, 0), min(rawLineCount, renderedLines.count) - 1)
        }
        var lineStartOffsets: [Int] = []
        var runningOffset = 0
        for (index, line) in renderedLines.enumerated() {
            lineStartOffsets.append(runningOffset)
            result.append(line)
            runningOffset += line.length
            if index < renderedLines.count - 1 {
                result.append(NSAttributedString(string: "\n"))
                runningOffset += 1
            }
        }
        let cursorInfo: TerminalCursorInfo?
        if let row = resolvedCursorRow,
           row < rawLineCount,
           row < lineStartOffsets.count {
            let clampedColumn = snapshot.clampedColumn(row: row, column: cursorCol)
            let columnOffset = snapshot.utf16Offset(row: row, column: clampedColumn)
            let totalOffset = min(lineStartOffsets[row] + columnOffset, result.length)
            cursorInfo = TerminalCursorInfo(row: row, column: clampedColumn, textOffset: totalOffset)
        } else {
            cursorInfo = nil
        }
        return (result, cursorInfo)
    }
}

struct TerminalCursorInfo: Equatable {
    let row: Int
    let column: Int
    let textOffset: Int
}

struct ElvisSnapshot {
    let text: String
    let cursor: TerminalCursorInfo?
}

final class ElvisTerminalBridge {
    static let shared = ElvisTerminalBridge()

    private struct ScreenState {
        var rows: Int = 0
        var columns: Int = 0
        var grid: [[Character]] = []
        var cursorRow: Int = 0
        var cursorCol: Int = 0
        var active: Bool = false
    }

    private let stateQueue = DispatchQueue(label: "com.pscal.elvis.bridge.state", attributes: .concurrent)
    private var state = ScreenState()
    private let inputCondition = NSCondition()
    private var pendingInput: [UInt8] = []

    var isActive: Bool {
        stateQueue.sync { state.active }
    }

    func activate(columns: Int, rows: Int) {
        stateQueue.sync(flags: .barrier) {
            state.active = true
            state.columns = max(1, columns)
            state.rows = max(1, rows)
            let blankRow = Array(repeating: Character(" "), count: state.columns)
            state.grid = Array(repeating: blankRow, count: state.rows)
            state.cursorRow = 0
            state.cursorCol = 0
        }
        inputCondition.lock()
        pendingInput.removeAll()
        inputCondition.unlock()
    }

    func deactivate() {
        stateQueue.sync(flags: .barrier) {
            state.active = false
            state.grid.removeAll()
            state.rows = 0
            state.columns = 0
            state.cursorRow = 0
            state.cursorCol = 0
        }
        inputCondition.lock()
        pendingInput.removeAll()
        inputCondition.broadcast()
        inputCondition.unlock()
    }

    func resize(columns: Int, rows: Int) {
        stateQueue.sync(flags: .barrier) {
            guard state.active else { return }
            let newRows = max(1, rows)
            let newCols = max(1, columns)
            let blankRow = Array(repeating: Character(" "), count: newCols)
            var newGrid = Array(repeating: blankRow, count: newRows)
            let copyRows = min(newRows, state.rows)
            let copyCols = min(newCols, state.columns)
            for r in 0..<copyRows {
                for c in 0..<copyCols {
                    newGrid[r][c] = state.grid[r][c]
                }
            }
            state.grid = newGrid
            state.rows = newRows
            state.columns = newCols
            state.cursorRow = min(state.cursorRow, newRows - 1)
            state.cursorCol = min(state.cursorCol, newCols - 1)
        }
    }

    func clear() {
        stateQueue.sync(flags: .barrier) {
            guard state.active else { return }
            let blankRow = Array(repeating: Character(" "), count: state.columns)
            state.grid = Array(repeating: blankRow, count: state.rows)
            state.cursorRow = 0
            state.cursorCol = 0
        }
    }

    func moveCursor(row: Int, column: Int) {
        stateQueue.sync(flags: .barrier) {
            guard state.active, state.rows > 0, state.columns > 0 else { return }
            state.cursorRow = max(0, min(row, state.rows - 1))
            state.cursorCol = max(0, min(column, state.columns - 1))
        }
    }

    private func normalizeRow(_ row: Int) -> Int {
        guard state.rows > 0 else { return 0 }
        var normalized = row
        while normalized < 0 {
            normalized += state.rows
        }
        return normalized % max(1, state.rows)
    }

    func draw(row: Int, column: Int, text: UnsafePointer<CChar>?, length: Int) {
        guard let text, length > 0 else { return }
        let buffer = UnsafeBufferPointer(start: text, count: length)
        stateQueue.sync(flags: .barrier) {
            guard state.active, state.rows > 0, state.columns > 0 else { return }
            let targetRow = max(0, min(normalizeRow(row), state.rows - 1))
            var targetCol = max(0, min(column, state.columns - 1))
            for byte in buffer {
                if targetCol >= state.columns {
                    break
                }
                let scalarValue = UInt32(UInt8(bitPattern: byte))
                let scalar = UnicodeScalar(scalarValue) ?? " "
                state.grid[targetRow][targetCol] = Character(scalar)
                targetCol += 1
            }
        }
    }

    func clearToEndOfLine(row: Int, column: Int) {
        stateQueue.sync(flags: .barrier) {
            guard state.active, state.rows > 0, state.columns > 0 else { return }
            let targetRow = max(0, min(row, state.rows - 1))
            let startCol = max(0, min(column, state.columns))
            let space = Character(" ")
            for col in startCol..<state.columns {
                state.grid[targetRow][col] = space
            }
        }
    }

    func snapshot() -> ElvisSnapshot {
        let currentState = stateQueue.sync { state }
        let lines: [String]
        if currentState.rows == 0 || currentState.columns == 0 || currentState.grid.isEmpty {
            lines = [""]
        } else {
            lines = currentState.grid.map { String($0) }
        }
        let joined = lines.joined(separator: "\n")
        let cursor: TerminalCursorInfo?
        if currentState.active && !lines.isEmpty {
            let row = max(0, min(currentState.cursorRow, lines.count - 1))
            let column = max(0, min(currentState.cursorCol, currentState.columns))
            var offset = 0
            if row > 0 {
                for idx in 0..<row {
                    offset += lines[idx].utf16.count + 1
                }
            }
            let line = lines[row]
            let clampedColumn = min(column, line.count)
            var columnOffset = 0
            if clampedColumn > 0 {
                var consumed = 0
                for ch in line {
                    if consumed >= clampedColumn {
                        break
                    }
                    columnOffset += String(ch).utf16.count
                    consumed += 1
                }
            }
            offset += columnOffset
            cursor = TerminalCursorInfo(row: row, column: clampedColumn, textOffset: offset)
        } else {
            cursor = nil
        }
        return ElvisSnapshot(text: joined,
                              cursor: cursor)
    }

    func debugState() -> (active: Bool, rows: Int, columns: Int) {
        return stateQueue.sync {
            (state.active, state.rows, state.columns)
        }
    }

    func interceptInputIfNeeded(data: Data) -> Bool {
        guard isActive else { return false }
        inputCondition.lock()
        pendingInput.append(contentsOf: data)
        inputCondition.signal()
        inputCondition.unlock()
        return true
    }

    func read(into buffer: UnsafeMutablePointer<UInt8>, maxLength: Int, timeoutMs: Int) -> Int {
        guard maxLength > 0 else { return 0 }
        inputCondition.lock()
        defer { inputCondition.unlock() }
        let timeoutSeconds = Double(max(timeoutMs, 0)) / 10.0
        let deadline = timeoutMs > 0 ? Date().addingTimeInterval(timeoutSeconds) : nil
        while pendingInput.isEmpty {
            if !isActive {
                return 0
            }
            if let deadline {
                if !inputCondition.wait(until: deadline) {
                    break
                }
            } else {
                inputCondition.wait()
            }
        }
        if pendingInput.isEmpty {
            return 0
        }
        let count = min(maxLength, pendingInput.count)
        for idx in 0..<count {
            buffer[idx] = pendingInput[idx]
        }
        pendingInput.removeFirst(count)
        return count
    }
}

@_cdecl("pscalTerminalBegin")
func pscalTerminalBegin(_ columns: Int32, _ rows: Int32) {
    let logLine = "pscalTerminalBegin cols=\(columns) rows=\(rows)"
    runtimeDebugLog(logLine)
    ElvisTerminalBridge.shared.activate(columns: Int(columns), rows: Int(rows))
    PscalRuntimeBootstrap.shared.setElvisModeActive(true)
    ElvisWindowManager.shared.showWindow()
}

@_cdecl("pscalTerminalEnd")
func pscalTerminalEnd() {
    runtimeDebugLog("pscalTerminalEnd")
    ElvisTerminalBridge.shared.deactivate()
    PscalRuntimeBootstrap.shared.setElvisModeActive(false)
    ElvisWindowManager.shared.hideWindow()
}

@_cdecl("pscalTerminalResize")
func pscalTerminalResize(_ columns: Int32, _ rows: Int32) {
    let logLine = "pscalTerminalResize cols=\(columns) rows=\(rows)"
    runtimeDebugLog(logLine)
    ElvisTerminalBridge.shared.resize(columns: Int(columns), rows: Int(rows))
    PscalRuntimeBootstrap.shared.refreshElvisDisplay()
}

@_cdecl("pscalTerminalRender")
func pscalTerminalRender(_ utf8: UnsafePointer<CChar>?, _ len: Int32, _ row: Int32, _ col: Int32, _ fg: Int64, _ bg: Int64, _ attr: Int32) {
    let logLine = "pscalTerminalRender len=\(len) row=\(row) col=\(col)"
    runtimeDebugLog(logLine)
    _ = fg
    _ = bg
    _ = attr
    ElvisTerminalBridge.shared.draw(row: Int(row), column: Int(col), text: utf8, length: Int(len))
    PscalRuntimeBootstrap.shared.refreshElvisDisplay()
}

@_cdecl("pscalTerminalClear")
func pscalTerminalClear() {
    runtimeDebugLog("pscalTerminalClear")
    ElvisTerminalBridge.shared.clear()
    PscalRuntimeBootstrap.shared.refreshElvisDisplay()
}

@_cdecl("pscalTerminalMoveCursor")
func pscalTerminalMoveCursor(_ row: Int32, _ column: Int32) {
    ElvisTerminalBridge.shared.moveCursor(row: Int(row), column: Int(column))
    PscalRuntimeBootstrap.shared.refreshElvisDisplay()
}

@_cdecl("pscalTerminalClearEol")
func pscalTerminalClearEol(_ row: Int32, _ column: Int32) {
    ElvisTerminalBridge.shared.clearToEndOfLine(row: Int(row), column: Int(column))
    PscalRuntimeBootstrap.shared.refreshElvisDisplay()
}

@_cdecl("pscalTerminalRead")
func pscalTerminalRead(_ buffer: UnsafeMutablePointer<UInt8>?, _ maxlen: Int32, _ timeout: Int32) -> Int32 {
    guard let buffer else { return 0 }
    let bytesRead = ElvisTerminalBridge.shared.read(into: buffer, maxLength: Int(maxlen), timeoutMs: Int(timeout))
    return Int32(bytesRead)
}

@_cdecl("pscalElvisDump")
func pscalElvisDump() {
    let snapshot = ElvisTerminalBridge.shared.snapshot()
    snapshot.text.withCString { cstr in
        fputs(cstr, stdout)
        fputc(0x0A, stdout)
    }
    let debugState = ElvisTerminalBridge.shared.debugState()
    if let cursor = snapshot.cursor {
        let cursorLine = "[elvisdump] cursor row=\(cursor.row) col=\(cursor.column)\n"
        cursorLine.withCString { fputs($0, stderr) }
    } else {
        fputs("[elvisdump] cursor unavailable\n", stderr)
    }
    let stateLine = "[elvisdump] active=\(debugState.active) rows=\(debugState.rows) cols=\(debugState.columns)\n"
    stateLine.withCString { fputs($0, stderr) }
}
