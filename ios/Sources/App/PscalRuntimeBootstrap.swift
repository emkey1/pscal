import Combine
import Foundation
import Darwin
import UIKit

final class PscalRuntimeBootstrap: ObservableObject {
    static let shared = PscalRuntimeBootstrap()

    @Published private(set) var screenText: NSAttributedString = NSAttributedString(string: "Launching exsh...")
    @Published private(set) var exitStatus: Int32?
    @Published private(set) var cursorInfo: TerminalCursorInfo?
    @Published private(set) var terminalBackgroundColor: UIColor = UIColor.systemBackground

    private var started = false
    private let stateQueue = DispatchQueue(label: "com.pscal.runtime.state")
    private let launchQueue = DispatchQueue(label: "com.pscal.runtime.launch", qos: .userInitiated)
    // Dedicated pthread stack for the runtime (libdispatch workers are ~512KB).
    private let runtimeStackSizeBytes = 4 * 1024 * 1024
    private var handlerContext: UnsafeMutableRawPointer?
    private let terminalBuffer: TerminalBuffer
    private var lastAppliedGeometry: (columns: Int, rows: Int)
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
        let initialColumns = 80
        let initialRows = 24
        self.terminalBuffer = TerminalBuffer(columns: initialColumns,
                                             rows: initialRows,
                                             scrollback: 400,
                                             dsrResponder: { data in
            data.withUnsafeBytes { buffer in
                guard let base = buffer.baseAddress else { return }
                let pointer = base.assumingMemoryBound(to: CChar.self)
                PSCALRuntimeSendInput(pointer, buffer.count)
            }
        })
        self.lastAppliedGeometry = (initialColumns, initialRows)
        PSCALRuntimeUpdateWindowSize(Int32(initialColumns), Int32(initialRows))
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

        DispatchQueue.main.async {
            self.screenText = NSAttributedString(string: "Launching exsh...")
            self.exitStatus = nil
            self.terminalBackgroundColor = UIColor.systemBackground
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
        echoLocallyIfNeeded(text)
        data.withUnsafeBytes { rawBuffer in
            guard let base = rawBuffer.bindMemory(to: CChar.self).baseAddress else { return }
            PSCALRuntimeSendInput(base, rawBuffer.count)
        }
    }

    func updateTerminalSize(columns: Int, rows: Int) {
        let clampedColumns = max(10, columns)
        let clampedRows = max(4, rows)
        guard clampedColumns != lastAppliedGeometry.columns || clampedRows != lastAppliedGeometry.rows else {
            return
        }
        lastAppliedGeometry = (clampedColumns, clampedRows)
        let resized = terminalBuffer.resize(columns: clampedColumns, rows: clampedRows)
        PSCALRuntimeUpdateWindowSize(Int32(clampedColumns), Int32(clampedRows))
        if resized {
            scheduleRender()
        }
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
        stateQueue.async {
            self.started = false
        }
        DispatchQueue.main.async {
            self.exitStatus = status
        }
    }

    private func scheduleRender(preserveBackground: Bool = false) {
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

    private static func renderJoined(snapshot: TerminalBuffer.TerminalSnapshot) -> (text: NSAttributedString, cursor: TerminalCursorInfo?) {
        let lines = TerminalBuffer.render(snapshot: snapshot)
        let result = NSMutableAttributedString()
        let cursorRow = snapshot.cursor?.row
        let cursorCol = snapshot.cursor?.col ?? 0
        var resolvedCursorRow: Int?
        if let row = cursorRow, !lines.isEmpty {
            resolvedCursorRow = min(max(row, 0), lines.count - 1)
        }
        for (index, line) in lines.enumerated() {
            result.append(line)
            if index < lines.count - 1 {
                result.append(NSAttributedString(string: "\n"))
            }
        }
        let cursorInfo: TerminalCursorInfo?
        if let row = resolvedCursorRow {
            let line = lines[row]
            let clampedColumn = min(cursorCol, line.length)
            cursorInfo = TerminalCursorInfo(row: row, column: clampedColumn)
        } else {
            cursorInfo = nil
        }
        return (result, cursorInfo)
    }
}

struct TerminalCursorInfo: Equatable {
    let row: Int
    let column: Int
}
