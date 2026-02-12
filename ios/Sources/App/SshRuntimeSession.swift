import Foundation
import UIKit
import Darwin

final class SshRuntimeSession: ObservableObject {
    let sessionId: UInt64
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
    private let stateQueue = DispatchQueue(label: "com.pscal.ssh.session.state")
    private let inputQueue = DispatchQueue(label: "com.pscal.ssh.session.input", qos: .userInitiated)
    private let outputQueue = DispatchQueue(label: "com.pscal.ssh.session.output", qos: .utility)
    private var renderQueued = false
    private var lastRenderTime: TimeInterval = 0
    private let minRenderInterval: TimeInterval = 0.03
    private var detachedOutputChunks: [Data] = []
    private var detachedOutputHead: Int = 0
    private var detachedOutputBytes: Int = 0
    private let detachedOutputMaxBytes: Int = 2 * 1024 * 1024
    private var started = false
    private var didExit = false
    private var pendingColumns: Int
    private var pendingRows: Int
    private var runtimeContext: OpaquePointer?
    private var handlerContext: UnsafeMutableRawPointer?
    private let outputHandlerGroup = DispatchGroup()
    let htermController: HtermTerminalController
    private var htermAttached = false
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
        let unmanaged = Unmanaged<SshRuntimeSession>.fromOpaque(context)
        let session = unmanaged.takeUnretainedValue()
        session.outputHandlerGroup.enter()
        session.consumeOutput(buffer: data, length: Int(length))
        session.outputHandlerGroup.leave()
    }

    init(sessionId: UInt64, argv: [String]) {
        self.sessionId = sessionId
        self.argv = argv
        self.runtimeContext = PSCALRuntimeCreateRuntimeContext()
        let metrics = PscalRuntimeBootstrap.defaultGeometryMetrics()
        self.pendingColumns = metrics.columns
        self.pendingRows = metrics.rows
        self.screenText = NSAttributedString(string: "Launching ssh...")
        self.htermController = HtermTerminalController.makeOnMainThread()
        _ = terminalBuffer
    }

    deinit {
        if let runtimeContext = runtimeContext {
            PSCALRuntimeDestroyRuntimeContext(runtimeContext)
        }
    }

    func attachHtermController(_ controller: HtermTerminalController?) {
        outputQueue.async {
            guard let controller else {
                Self.logHterm("Hterm: attach ssh session=\(self.sessionId) (nil controller)")
                return
            }
            guard controller === self.htermController else {
                Self.logHterm("Hterm[\(controller.instanceId)]: attach ssh session=\(self.sessionId) ignored (unexpected controller)")
                return
            }
            Self.logHterm("Hterm[\(controller.instanceId)]: attach ssh session=\(self.sessionId)")
            self.htermAttached = true
            self.flushDetachedOutputIfNeeded()
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
            Self.logHterm("Hterm[\(controller.instanceId)]: detach ssh session=\(self.sessionId)")
            self.htermAttached = false
            DispatchQueue.main.async {
                controller.setResizeSessionId(0)
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

        sshDebugLog("[ssh-session] start id=\(sessionId)")
        withRuntimeContext {
            PSCALRuntimeRegisterSessionContext(sessionId)
        }
        handlerContext = Unmanaged.passRetained(self).toOpaque()
        withRuntimeContext {
            PSCALRuntimeRegisterSessionOutputHandler(sessionId, sessionOutputHandler, handlerContext)
        }
        if Self.ioDebugEnabled {
            let ctxDesc = runtimeContext.map { String(describing: $0) } ?? "nil"
            let message = "SshSession: start id=\(sessionId) ctx=\(ctxDesc)\n"
            if let data = message.data(using: .utf8) {
                FileHandle.standardError.write(data)
            }
        }

        var readFd: Int32 = -1
        var writeFd: Int32 = -1
        let launched = launchSshSession(readFd: &readFd, writeFd: &writeFd)
        if !launched {
            let err = errno
            lastStartErrno = err == 0 ? EIO : err
            sshDebugLog("[ssh-session] start failed id=\(sessionId) errno=\(lastStartErrno)")
            stopOutputHandler()
            closeIfValid(readFd)
            closeIfValid(writeFd)
            markExited(status: 255)
            return false
        }
        sshDebugLog("[ssh-session] launched id=\(sessionId) readFd=\(readFd) writeFd=\(writeFd)")
        if Self.ioDebugEnabled {
            let message = "SshSession: launched id=\(sessionId) readFd=\(readFd) writeFd=\(writeFd)\n"
            if let data = message.data(using: .utf8) {
                FileHandle.standardError.write(data)
            }
        }
        lastStartErrno = 0
        applyPendingWinsize()
        closeIfValid(readFd)
        closeIfValid(writeFd)
        return true
    }

    func send(_ text: String) {
        guard !text.isEmpty else { return }
        sendData(text.data(using: .utf8) ?? Data())
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
        send("\u{04}")
    }

    func updateTerminalSize(columns: Int, rows: Int) {
        let clampedColumns = max(10, columns)
        let clampedRows = max(4, rows)
        sshResizeLog("[ssh-resize] session update id=\(sessionId) req=\(columns)x\(rows) clamped=\(clampedColumns)x\(clampedRows)")
        pendingColumns = clampedColumns
        pendingRows = clampedRows
        if terminalBuffer.resize(columns: clampedColumns, rows: clampedRows) {
            scheduleRender()
        }
        DispatchQueue.main.async { [weak self] in
            sshResizeLog("[ssh-resize] session force-grid id=\(self?.sessionId ?? 0) cols=\(clampedColumns) rows=\(clampedRows)")
            self?.htermController.forceGridSize(columns: clampedColumns, rows: clampedRows)
        }
        withRuntimeContext {
            PSCALRuntimeUpdateSessionWindowSize(sessionId, Int32(clampedColumns), Int32(clampedRows))
        }
    }

    func markExited(status: Int32?) {
        let shouldUpdate = stateQueue.sync { () -> Bool in
            guard !didExit else { return false }
            didExit = true
            return true
        }
        guard shouldUpdate else { return }
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
    }

    // MARK: - Output Handler

    private func stopOutputHandler() {
        guard let context = handlerContext else { return }
        withRuntimeContext {
            PSCALRuntimeUnregisterSessionOutputHandler(sessionId)
        }
        outputHandlerGroup.wait()
        Unmanaged<SshRuntimeSession>.fromOpaque(context).release()
        handlerContext = nil
    }

    // MARK: - Rendering

    private func scheduleRender() {
        if htermAttached {
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
        if htermAttached {
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

    private func applyPendingWinsize() {
        let cols = pendingColumns
        let rows = pendingRows
        guard cols > 0, rows > 0 else { return }
        sshResizeLog("[ssh-resize] session apply-pending id=\(sessionId) cols=\(cols) rows=\(rows)")
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
                self.flushDetachedOutputIfNeeded()
                self.htermController.enqueueOutput(data)
                self.terminalBuffer.append(data: data)
            } else {
                self.queueDetachedOutput(data)
            }
        }
    }

    private func queueDetachedOutput(_ data: Data) {
        guard !data.isEmpty else { return }
        guard detachedOutputMaxBytes > 0 else { return }
        if data.count >= detachedOutputMaxBytes {
            let tail = data.suffix(detachedOutputMaxBytes)
            detachedOutputChunks = [Data(tail)]
            detachedOutputHead = 0
            detachedOutputBytes = tail.count
            return
        }

        detachedOutputChunks.append(data)
        detachedOutputBytes += data.count
        while detachedOutputBytes > detachedOutputMaxBytes && detachedOutputHead < detachedOutputChunks.count {
            detachedOutputBytes -= detachedOutputChunks[detachedOutputHead].count
            detachedOutputHead += 1
        }
        if detachedOutputHead > 64 && detachedOutputHead > detachedOutputChunks.count / 2 {
            detachedOutputChunks.removeFirst(detachedOutputHead)
            detachedOutputHead = 0
        }
    }

    private func flushDetachedOutputIfNeeded() {
        guard htermAttached else { return }
        guard detachedOutputHead < detachedOutputChunks.count else {
            detachedOutputChunks.removeAll(keepingCapacity: true)
            detachedOutputHead = 0
            detachedOutputBytes = 0
            return
        }

        let slice = detachedOutputChunks[detachedOutputHead...]
        var merged = Data()
        merged.reserveCapacity(detachedOutputBytes)
        for chunk in slice {
            merged.append(chunk)
        }
        detachedOutputChunks.removeAll(keepingCapacity: true)
        detachedOutputHead = 0
        detachedOutputBytes = 0
        guard !merged.isEmpty else { return }
        htermController.enqueueOutput(merged)
        terminalBuffer.append(data: merged)
    }

    private func launchSshSession(readFd: inout Int32, writeFd: inout Int32) -> Bool {
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
                PSCALRuntimeCreateSshSession(argc,
                                             base,
                                             sessionId,
                                             &readFd,
                                             &writeFd)
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
