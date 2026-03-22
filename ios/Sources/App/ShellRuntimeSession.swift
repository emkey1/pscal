import Foundation
import UIKit
import Darwin

final class ShellRuntimeSession: ObservableObject {
    private final class WeakSessionRef {
        weak var value: ShellRuntimeSession?

        init(_ value: ShellRuntimeSession) {
            self.value = value
        }
    }

    enum SessionProgram {
        case shell
        case ssh

        var displayName: String {
            switch self {
            case .shell:
                return "Shell"
            case .ssh:
                return "SSH"
            }
        }

        var launchLabel: String {
            switch self {
            case .shell:
                return "shell"
            case .ssh:
                return "ssh"
            }
        }
    }

    let sessionId: UInt64
    let sessionDisplayName: String
    private static let htermDebugEnabled: Bool = {
        let env = ProcessInfo.processInfo.environment
        if let value = env["PSCALI_HERM_DEBUG"] {
            return value != "0"
        }
        if let value = env["PSCALI_PTY_OUTPUT_LOG"] {
            return value != "0"
        }
        return false
    }()
    private static let ioDebugEnabled: Bool = {
        let env = ProcessInfo.processInfo.environment
        if let value = env["PSCALI_IO_DEBUG"] {
            return value != "0"
        }
        return false
    }()
    private static let standaloneRegistryLock = NSLock()
    private static var standaloneSessions: [UInt64: WeakSessionRef] = [:]

    static func handleStandaloneSessionExit(sessionId: UInt64, status: Int32) -> Bool {
        standaloneRegistryLock.lock()
        let ref = standaloneSessions[sessionId]
        let session = ref?.value
        if ref != nil && session == nil {
            standaloneSessions.removeValue(forKey: sessionId)
        }
        standaloneRegistryLock.unlock()
        guard let session else {
            return false
        }
        session.markExited(status: status)
        return true
    }

    private static func registerStandaloneSession(_ session: ShellRuntimeSession) {
        standaloneRegistryLock.lock()
        standaloneSessions[session.sessionId] = WeakSessionRef(session)
        standaloneRegistryLock.unlock()
    }

    private static func unregisterStandaloneSession(sessionId: UInt64) {
        standaloneRegistryLock.lock()
        standaloneSessions.removeValue(forKey: sessionId)
        standaloneRegistryLock.unlock()
    }

    private static func logHterm(_ message: String) {
        guard htermDebugEnabled else { return }
        if let data = (message + "\n").data(using: .utf8) {
            FileHandle.standardError.write(data)
        }
    }

    @Published private(set) var screenText: NSAttributedString
    @Published private(set) var cursorInfo: TerminalCursorInfo?
    @Published private(set) var exitStatus: Int32?
    @Published private(set) var mouseMode: TerminalBuffer.MouseMode = .none
    @Published private(set) var mouseEncoding: TerminalBuffer.MouseEncoding = .normal
    private(set) var lastStartErrno: Int32 = 0

    private let argv: [String]
    private let sessionProgram: SessionProgram
    private let sessionLogLabel: String
    private let managedByTabManager: Bool
    private lazy var terminalBuffer: TerminalBuffer = {
        let buffer = TerminalBuffer(columns: pendingColumns,
                                    rows: pendingRows,
                                    scrollback: 400,
                                    dsrResponder: { [weak self] data in
            self?.sendData(data)
        })
        buffer.setResizeHandler { [weak self] columns, rows in
            self?.updateTerminalSize(columns: columns, rows: rows)
        }
        buffer.onMouseModeChange = { [weak self] mode, encoding in
            self?.mouseMode = mode
            self?.mouseEncoding = encoding
        }
        return buffer
    }()
    private let stateQueue = DispatchQueue(label: "com.pscal.shell.session.state")
    private let inputQueue = DispatchQueue(label: "com.pscal.shell.session.input", qos: .userInitiated)
    private let outputQueue = DispatchQueue(label: "com.pscal.shell.session.output", qos: .utility)
    private var renderQueued = false
    private var lastRenderTime: TimeInterval = 0
    private let minRenderInterval: TimeInterval = 0.016
    private var attachedOutputChunks: [Data] = []
    private var attachedOutputHead: Int = 0
    private var attachedOutputBytes: Int = 0
    private var attachedFlushScheduled = false
    private let attachedFlushInterval: TimeInterval = 0.001
    private let attachedFlushChunkBytes: Int = 128 * 1024
    private let attachedOutputPauseHighWatermark: Int = 512 * 1024
    private let attachedOutputPauseLowWatermark: Int = 128 * 1024
    private var outputPausedForBackpressure = false
    private var sessionOutputPaused = false
    private var pauseTransitionCount = 0
    private var started = false
    private var didExit = false
    private var pendingColumns: Int
    private var pendingRows: Int
    private var runtimeContext: OpaquePointer?
    private var handlerContext: UnsafeMutableRawPointer?
    private let outputHandlerGroup = DispatchGroup()
    let htermController: HtermTerminalController
    private var htermAttached = false
    private var viewVisible = false
    private let metricsLogger: TerminalSessionMetricsLogger
    private func withRuntimeContext<T>(_ body: () -> T) -> T {
        guard let runtimeContext else {
            return body()
        }
        let previous = PSCALRuntimeGetCurrentRuntimeContext()
        PSCALRuntimeSetCurrentRuntimeContext(runtimeContext)
        defer { PSCALRuntimeSetCurrentRuntimeContext(previous) }
        return body()
    }
    private lazy var sessionOutputHandler: PSCALRuntimeSessionOutputHandler = { sessionId, data, length, context in
        guard let context, let data else { return }
        let unmanaged = Unmanaged<ShellRuntimeSession>.fromOpaque(context)
        let session = unmanaged.takeUnretainedValue()
        session.outputHandlerGroup.enter()
        session.consumeOutput(buffer: data, length: Int(length))
        session.outputHandlerGroup.leave()
    }

    init(sessionId: UInt64,
         argv: [String],
         program: SessionProgram = .shell,
         managedByTabManager: Bool = false) {
        self.sessionId = sessionId
        self.argv = argv
        self.sessionProgram = program
        self.sessionDisplayName = program.displayName
        self.sessionLogLabel = program.launchLabel
        self.managedByTabManager = managedByTabManager
        self.runtimeContext = PSCALRuntimeCreateRuntimeContext()
        let metrics = PscalRuntimeBootstrap.defaultGeometryMetrics()
        self.pendingColumns = metrics.columns
        self.pendingRows = metrics.rows
        self.screenText = NSAttributedString(string: "Launching \(sessionLogLabel)...")
        self.htermController = HtermTerminalController.makeOnMainThread()
        self.metricsLogger = TerminalSessionMetricsLogger(sessionKind: sessionLogLabel, sessionId: sessionId)
        _ = terminalBuffer
        if !managedByTabManager {
            Self.registerStandaloneSession(self)
        }
    }

    deinit {
        if !managedByTabManager {
            Self.unregisterStandaloneSession(sessionId: sessionId)
        }
        if let runtimeContext = runtimeContext {
            PSCALRuntimeDestroyRuntimeContext(runtimeContext)
        }
    }

    func setViewVisible(_ visible: Bool) {
        outputQueue.async {
            self.viewVisible = visible
            if visible && !self.htermAttached {
                self.scheduleRender()
            }
            self.recordMetricsLocked(reason: visible ? "view-visible" : "view-hidden", force: true)
        }
    }

    func attachHtermController(_ controller: HtermTerminalController?) {
        outputQueue.async {
            guard let controller else {
                Self.logHterm("Hterm: attach \(self.sessionLogLabel) session=\(self.sessionId) (nil controller)")
                return
            }
            guard controller === self.htermController else {
                Self.logHterm("Hterm[\(controller.instanceId)]: attach \(self.sessionLogLabel) session=\(self.sessionId) ignored (unexpected controller)")
                return
            }
            Self.logHterm("Hterm[\(controller.instanceId)]: attach \(self.sessionLogLabel) session=\(self.sessionId)")
            self.htermAttached = true
            self.drainAttachedOutputToTerminalLocked()
            self.reloadHtermFromTerminalStateLocked()
            self.updateSessionOutputPauseLocked()
            self.recordMetricsLocked(reason: "attach", force: true)
            DispatchQueue.main.async {
                controller.setResizeSessionId(self.sessionId)
            }
        }
    }

    func detachHtermController(_ controller: HtermTerminalController) {
        outputQueue.async {
            guard controller === self.htermController else {
                return
            }
            Self.logHterm("Hterm[\(controller.instanceId)]: detach \(self.sessionLogLabel) session=\(self.sessionId)")
            self.htermAttached = false
            self.drainAttachedOutputToTerminalLocked()
            self.outputPausedForBackpressure = false
            self.updateSessionOutputPauseLocked()
            self.recordMetricsLocked(reason: "detach", force: true)
            DispatchQueue.main.async {
                controller.setResizeSessionId(0)
                controller.discardPendingOutput()
            }
        }
    }

    @discardableResult
    func start() -> Bool {
        let shouldStart = stateQueue.sync { () -> Bool in
            guard !started else { return false }
            started = true
            return true
        }
        guard shouldStart else { return false }

        withRuntimeContext {
            PSCALRuntimeRegisterSessionContext(sessionId)
        }
        handlerContext = Unmanaged.passRetained(self).toOpaque()
        withRuntimeContext {
            PSCALRuntimeRegisterSessionOutputHandler(sessionId, sessionOutputHandler, handlerContext)
        }
        sessionOutputPaused = false
        updateSessionOutputPauseLocked()
        recordMetricsLocked(reason: "start", force: true)
        if Self.ioDebugEnabled {
            let ctxDesc = runtimeContext.map { String(describing: $0) } ?? "nil"
            let message = "Session(\(sessionLogLabel)): start id=\(sessionId) ctx=\(ctxDesc)\n"
            if let data = message.data(using: .utf8) {
                FileHandle.standardError.write(data)
            }
        }

        var readFd: Int32 = -1
        var writeFd: Int32 = -1
        let launched = launchSession(readFd: &readFd, writeFd: &writeFd)
        if !launched {
            let err = errno
            lastStartErrno = err == 0 ? EIO : err
            stopOutputHandler()
            closeIfValid(readFd)
            closeIfValid(writeFd)
            markExited(status: 255)
            return false
        }
        if Self.ioDebugEnabled {
            let message = "Session(\(sessionLogLabel)): launched id=\(sessionId) readFd=\(readFd) writeFd=\(writeFd)\n"
            if let data = message.data(using: .utf8) {
                FileHandle.standardError.write(data)
            }
        }
        lastStartErrno = 0
        applyPendingWinsize()
        closeIfValid(readFd)
        closeIfValid(writeFd)
        outputQueue.async { [weak self] in
            self?.recordMetricsLocked(reason: "launched", force: true)
        }
        return true
    }

    func send(_ text: String) {
        guard !text.isEmpty else { return }
        sendData(text.data(using: .utf8) ?? Data())
    }

    func sendInterrupt() {
        // Match terminal semantics: inject ETX into the session PTY stream.
        sendControlByte(0x03)
    }

    func sendSuspend() {
        // Match terminal semantics: inject SUB into the session PTY stream.
        sendControlByte(0x1A)
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

    func requestClose() {
        let interrupted = withRuntimeContext {
            PSCALRuntimeSendSignalForSession(sessionId, SIGINT) != 0
        }
        if !interrupted {
            withRuntimeContext {
                PSCALRuntimeSendSignal(SIGINT)
            }
        }
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.2) { [self] in
            guard self.exitStatus == nil else { return }
            self.send("\u{04}")
        }
        DispatchQueue.main.asyncAfter(deadline: .now() + 1.0) { [self] in
            guard self.exitStatus == nil else { return }
            self.withRuntimeContext {
                _ = PSCALRuntimeSendSignalForSession(self.sessionId, SIGTERM)
            }
        }
    }

    func updateTerminalSize(columns: Int, rows: Int) {
        let clampedColumns = max(10, columns)
        let clampedRows = max(4, rows)
        pendingColumns = clampedColumns
        pendingRows = clampedRows
        if terminalBuffer.resize(columns: clampedColumns, rows: clampedRows) {
            scheduleRender()
        }
        DispatchQueue.main.async { [weak self] in
            self?.htermController.noteRuntimeSize(columns: clampedColumns, rows: clampedRows)
            self?.htermController.forceGridSize(columns: clampedColumns, rows: clampedRows)
        }
        withRuntimeContext {
            PSCALRuntimeUpdateSessionWindowSize(sessionId, Int32(clampedColumns), Int32(clampedRows))
        }
    }

    func terminalBufferMetrics() -> TerminalBuffer.BufferMetrics {
        return outputQueue.sync {
            terminalBuffer.metrics()
        }
    }

    func markExited(status: Int32?) {
        let shouldUpdate = stateQueue.sync { () -> Bool in
            guard !didExit else { return false }
            didExit = true
            return true
        }
        guard shouldUpdate else { return }
        if !managedByTabManager {
            Self.unregisterStandaloneSession(sessionId: sessionId)
        }
        stopOutputHandler()
        withRuntimeContext {
            PSCALRuntimeUnregisterSessionContext(sessionId)
        }
        if let runtimeContext = runtimeContext {
            PSCALRuntimeDestroyRuntimeContext(runtimeContext)
            self.runtimeContext = nil
        }
        if let status {
            DispatchQueue.main.async {
                self.exitStatus = status
            }
        }
        outputQueue.async { [weak self] in
            self?.recordMetricsLocked(reason: "exit", force: true)
        }
    }

    // MARK: - Output Handler

    private func stopOutputHandler() {
        guard let context = handlerContext else { return }
        withRuntimeContext {
            PSCALRuntimeUnregisterSessionOutputHandler(sessionId)
        }
        outputHandlerGroup.wait()
        Unmanaged<ShellRuntimeSession>.fromOpaque(context).release()
        handlerContext = nil
    }

    // MARK: - Rendering

    private func scheduleRender() {
        if htermAttached || !viewVisible {
            return
        }
        if renderQueued {
            return
        }
        let now = Date().timeIntervalSince1970
        let timeSinceLast = now - lastRenderTime
        if timeSinceLast < minRenderInterval {
            renderQueued = true
            let delay = minRenderInterval - timeSinceLast
            DispatchQueue.main.asyncAfter(deadline: .now() + delay) { [weak self] in
                self?.performRender()
            }
        } else {
            renderQueued = true
            DispatchQueue.main.async { [weak self] in
                self?.performRender()
            }
        }
    }

    private func performRender() {
        if htermAttached || !viewVisible {
            renderQueued = false
            return
        }
        renderQueued = false
        lastRenderTime = Date().timeIntervalSince1970
        let snapshot = terminalBuffer.snapshot(includeScrollback: true)
        let renderResult = PscalRuntimeBootstrap.renderJoined(snapshot: snapshot)
        screenText = renderResult.text
        cursorInfo = renderResult.cursor
    }

    // MARK: - Helpers

    private func sendData(_ data: Data) {
        guard !data.isEmpty else { return }
        inputQueue.async { [weak self] in
            guard let self else { return }
            self.withRuntimeContext {
                data.withUnsafeBytes { buffer in
                    guard let base = buffer.baseAddress else { return }
                    let ptr = base.assumingMemoryBound(to: CChar.self)
                    PSCALRuntimeSendInputForSession(self.sessionId, ptr, buffer.count)
                }
            }
        }
    }

    private func sendControlByte(_ byte: UInt8) {
        inputQueue.async { [weak self] in
            guard let self else { return }
            self.withRuntimeContext {
                var value = CChar(bitPattern: byte)
                withUnsafePointer(to: &value) { ptr in
                    PSCALRuntimeSendInputUrgent(self.sessionId, ptr, 1)
                }
            }
        }
    }

    private func applyPendingWinsize() {
        let cols = pendingColumns
        let rows = pendingRows
        guard cols > 0, rows > 0 else { return }
        DispatchQueue.main.async { [weak self] in
            self?.htermController.noteRuntimeSize(columns: cols, rows: rows)
        }
        withRuntimeContext {
            PSCALRuntimeUpdateSessionWindowSize(sessionId, Int32(cols), Int32(rows))
        }
    }

    private func consumeOutput(buffer: UnsafePointer<CChar>, length: Int) {
        guard length > 0 else { return }
        let data = Data(bytes: buffer, count: length)
        outputQueue.async { [weak self] in
            guard let self else { return }
            if self.htermAttached {
                self.queueAttachedOutputLocked(data)
                self.scheduleAttachedFlushLocked()
                self.recordMetricsLocked(reason: "attached-queue")
            } else {
                self.terminalBuffer.append(data: data) { [weak self] in
                    self?.recordMetrics(reason: "terminal-append")
                    self?.scheduleRender()
                }
            }
        }
    }

    private func queueAttachedOutputLocked(_ data: Data) {
        guard !data.isEmpty else { return }
        attachedOutputChunks.append(data)
        attachedOutputBytes += data.count
        if !outputPausedForBackpressure && attachedOutputBytes >= attachedOutputPauseHighWatermark {
            outputPausedForBackpressure = true
            updateSessionOutputPauseLocked()
            recordMetricsLocked(reason: "backpressure-on", force: true)
        }
    }

    private func scheduleAttachedFlushLocked() {
        guard htermAttached else { return }
        guard attachedOutputBytes > 0 else { return }
        guard !attachedFlushScheduled else { return }
        attachedFlushScheduled = true
        outputQueue.asyncAfter(deadline: .now() + attachedFlushInterval) { [weak self] in
            self?.flushAttachedOutput()
        }
    }

    private func flushAttachedOutput() {
        guard htermAttached else {
            attachedFlushScheduled = false
            return
        }
        guard attachedOutputHead < attachedOutputChunks.count, attachedOutputBytes > 0 else {
            attachedFlushScheduled = false
            attachedOutputChunks.removeAll(keepingCapacity: true)
            attachedOutputHead = 0
            attachedOutputBytes = 0
            if outputPausedForBackpressure {
                outputPausedForBackpressure = false
                updateSessionOutputPauseLocked()
            }
            return
        }

        var remaining = min(attachedFlushChunkBytes, attachedOutputBytes)
        var merged = Data()
        merged.reserveCapacity(remaining)
        while remaining > 0 && attachedOutputHead < attachedOutputChunks.count {
            let chunk = attachedOutputChunks[attachedOutputHead]
            if chunk.count <= remaining {
                merged.append(chunk)
                remaining -= chunk.count
                attachedOutputHead += 1
            } else {
                merged.append(chunk.prefix(remaining))
                attachedOutputChunks[attachedOutputHead].removeFirst(remaining)
                remaining = 0
            }
        }

        let emittedBytes = merged.count
        if emittedBytes > 0 {
            attachedOutputBytes -= emittedBytes
            htermController.enqueueOutput(merged)
            terminalBuffer.append(data: merged) { [weak self] in
                self?.recordMetrics(reason: "attached-flush")
            }
        }

        if attachedOutputHead > 64 && attachedOutputHead > attachedOutputChunks.count / 2 {
            attachedOutputChunks.removeFirst(attachedOutputHead)
            attachedOutputHead = 0
        }

        if outputPausedForBackpressure && attachedOutputBytes <= attachedOutputPauseLowWatermark {
            outputPausedForBackpressure = false
            updateSessionOutputPauseLocked()
            recordMetricsLocked(reason: "backpressure-off", force: true)
        }

        attachedFlushScheduled = false
        if attachedOutputBytes > 0 {
            scheduleAttachedFlushLocked()
        }
    }

    private func drainAttachedOutputToTerminalLocked() {
        guard attachedOutputHead < attachedOutputChunks.count else {
            attachedOutputChunks.removeAll(keepingCapacity: true)
            attachedOutputHead = 0
            attachedOutputBytes = 0
            attachedFlushScheduled = false
            return
        }
        for index in attachedOutputHead..<attachedOutputChunks.count {
            terminalBuffer.append(data: attachedOutputChunks[index])
        }
        attachedOutputChunks.removeAll(keepingCapacity: true)
        attachedOutputHead = 0
        attachedOutputBytes = 0
        attachedFlushScheduled = false
    }

    private func reloadHtermFromTerminalStateLocked() {
        let snapshotData = terminalBuffer.vt100SnapshotData(includeScrollback: true)
        htermController.reloadFromSnapshot(snapshotData)
    }

    private func updateSessionOutputPauseLocked() {
        let shouldPause = htermAttached && outputPausedForBackpressure
        guard shouldPause != sessionOutputPaused else { return }
        sessionOutputPaused = shouldPause
        pauseTransitionCount += 1
        withRuntimeContext {
            PSCALRuntimeSetSessionOutputPaused(sessionId, shouldPause ? 1 : 0)
        }
        recordMetricsLocked(reason: shouldPause ? "session-pause" : "session-resume", force: true)
    }

    private func recordMetrics(reason: String, force: Bool = false) {
        outputQueue.async { [weak self] in
            self?.recordMetricsLocked(reason: reason, force: force)
        }
    }

    private func recordMetricsLocked(reason: String, force: Bool = false) {
        let snapshot = TerminalSessionMetricsSnapshot(visible: viewVisible,
                                                     htermAttached: htermAttached,
                                                     sessionOutputPaused: sessionOutputPaused,
                                                     attachedOutputBytes: attachedOutputBytes,
                                                     pauseTransitions: pauseTransitionCount,
                                                     terminalBuffer: terminalBuffer.metrics(),
                                                     htermOutput: htermController.outputMetrics())
        metricsLogger.record(snapshot, reason: reason, force: force)
    }

    private func launchSession(readFd: inout Int32, writeFd: inout Int32) -> Bool {
        guard !argv.isEmpty else { return false }
        var cStrings: [UnsafeMutablePointer<CChar>?] = argv.map { strdup($0) }
        cStrings.append(nil)
        defer {
            for ptr in cStrings {
                if let ptr {
                    free(ptr)
                }
            }
        }
        let argc = Int32(argv.count)
        let result = cStrings.withUnsafeMutableBufferPointer { buffer -> Int32 in
            guard let base = buffer.baseAddress else { return -1 }
            return withRuntimeContext {
                switch sessionProgram {
                case .shell:
                    return PSCALRuntimeCreateShellSession(argc,
                                                          base,
                                                          sessionId,
                                                          &readFd,
                                                          &writeFd)
                case .ssh:
                    return PSCALRuntimeCreateSshSession(argc,
                                                        base,
                                                        sessionId,
                                                        &readFd,
                                                        &writeFd)
                }
            }
        }
        return result == 0
    }

    private func closeIfValid(_ fd: Int32) {
        if fd >= 0 {
            close(fd)
        }
    }
}
