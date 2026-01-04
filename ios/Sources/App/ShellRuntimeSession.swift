import Foundation
import UIKit
import Darwin

final class ShellRuntimeSession: ObservableObject {
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
    private let stateQueue = DispatchQueue(label: "com.pscal.shell.session.state")
    private let inputQueue = DispatchQueue(label: "com.pscal.shell.session.input", qos: .userInitiated)
    private let outputQueue = DispatchQueue(label: "com.pscal.shell.session.output", qos: .utility)
    private var outputSource: DispatchSourceRead?
    private var renderQueued = false
    private var lastRenderTime: TimeInterval = 0
    private let minRenderInterval: TimeInterval = 0.03
    private var started = false
    private var didExit = false
    private var masterFd: Int32 = -1
    private var inputFd: Int32 = -1
    private var childFds: [Int32] = []
    private var usesPty = false
    private var pendingColumns: Int
    private var pendingRows: Int
    private var runtimeContext: OpaquePointer?
    private var handlerContext: UnsafeMutableRawPointer?
    private let outputHandlerGroup = DispatchGroup()
    private var directOutput = false
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
        let unmanaged = Unmanaged<ShellRuntimeSession>.fromOpaque(context)
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
        self.screenText = NSAttributedString(string: "Launching shell...")
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
                Self.logHterm("Hterm: attach shell session=\(self.sessionId) (nil controller)")
                return
            }
            guard controller === self.htermController else {
                Self.logHterm("Hterm[\(controller.instanceId)]: attach shell session=\(self.sessionId) ignored (unexpected controller)")
                return
            }
            Self.logHterm("Hterm[\(controller.instanceId)]: attach shell session=\(self.sessionId)")
            self.htermAttached = true
        }
    }

    func detachHtermController(_ controller: HtermTerminalController) {
        outputQueue.async {
            guard controller === self.htermController else {
                return
            }
            Self.logHterm("Hterm[\(controller.instanceId)]: detach shell session=\(self.sessionId)")
            self.htermAttached = false
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
        directOutput = usesDirectPtyOutput()
        if Self.ioDebugEnabled {
            let ctxDesc = runtimeContext.map { String(describing: $0) } ?? "nil"
            let message = "ShellSession: start id=\(sessionId) direct=\(directOutput ? 1 : 0) ctx=\(ctxDesc)\n"
            if let data = message.data(using: .utf8) {
                FileHandle.standardError.write(data)
            }
        }
        if directOutput {
            handlerContext = Unmanaged.passRetained(self).toOpaque()
            withRuntimeContext {
                PSCALRuntimeRegisterSessionOutputHandler(sessionId, sessionOutputHandler, handlerContext)
            }
        }

        var readFd: Int32 = -1
        var writeFd: Int32 = -1
        let launched = launchShellSession(readFd: &readFd, writeFd: &writeFd)
        if !launched {
            let err = errno
            lastStartErrno = err == 0 ? EIO : err
            stopOutputPump()
            directOutput = false
            closeIfValid(readFd)
            closeIfValid(writeFd)
            markExited(status: 255)
            return false
        }
        if Self.ioDebugEnabled {
            let message = "ShellSession: launched id=\(sessionId) readFd=\(readFd) writeFd=\(writeFd) direct=\(directOutput ? 1 : 0)\n"
            if let data = message.data(using: .utf8) {
                FileHandle.standardError.write(data)
            }
        }
        lastStartErrno = 0
        usesPty = true
        childFds.removeAll()
        if directOutput {
            closeIfValid(readFd)
            closeIfValid(writeFd)
            masterFd = -1
            inputFd = -1
        } else {
            masterFd = readFd
            inputFd = writeFd
            makeNonBlocking(fd: masterFd)
            makeNonBlocking(fd: inputFd)
            startOutputPump()
        }
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

    func updateTerminalSize(columns: Int, rows: Int) {
        let clampedColumns = max(10, columns)
        let clampedRows = max(4, rows)
        pendingColumns = clampedColumns
        pendingRows = clampedRows
        if terminalBuffer.resize(columns: clampedColumns, rows: clampedRows) {
            scheduleRender()
        }
        if usesPty {
            withRuntimeContext {
                _ = PSCALRuntimeSetSessionWinsize(sessionId, Int32(clampedColumns), Int32(clampedRows))
            }
        } else if masterFd >= 0 {
            applyWindowSize(fd: masterFd, columns: clampedColumns, rows: clampedRows)
        }
    }

    func markExited(status: Int32?) {
        let shouldUpdate = stateQueue.sync { () -> Bool in
            guard !didExit else { return false }
            didExit = true
            return true
        }
        guard shouldUpdate else { return }
        stopOutputPump()
        directOutput = false
        if inputFd >= 0 && inputFd != masterFd {
            closeIfValid(inputFd)
            inputFd = -1
        }
        for fd in childFds {
            closeIfValid(fd)
        }
        childFds.removeAll()
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

    // MARK: - Output Pump

    private func startOutputPump() {
        guard masterFd >= 0 else { return }
        let source = DispatchSource.makeReadSource(fileDescriptor: masterFd, queue: outputQueue)
        source.setEventHandler { [weak self] in
            self?.drainOutput()
        }
        source.setCancelHandler { [weak self] in
            guard let self else { return }
            if self.masterFd >= 0 {
                close(self.masterFd)
                self.masterFd = -1
            }
        }
        outputSource = source
        source.resume()
    }

    private func stopOutputPump() {
        outputSource?.cancel()
        outputSource = nil
        if directOutput, let context = handlerContext {
            withRuntimeContext {
                PSCALRuntimeUnregisterSessionOutputHandler(sessionId)
            }
            outputHandlerGroup.wait()
            Unmanaged<ShellRuntimeSession>.fromOpaque(context).release()
            handlerContext = nil
        }
    }

    private func drainOutput() {
        guard masterFd >= 0 else { return }
        var buffer = [UInt8](repeating: 0, count: 8192)
        var totalRead = 0
        var iterations = 0
        let maxIterations = 8
        let maxBytes = 64 * 1024
        while iterations < maxIterations && totalRead < maxBytes {
            let readCount = read(masterFd, &buffer, buffer.count)
            if readCount > 0 {
                totalRead += readCount
                iterations += 1
                let data = Data(buffer[0..<readCount])
                htermController.enqueueOutput(data)
                terminalBuffer.append(data: data)
                continue
            }
            if readCount == 0 {
                markExited(status: nil)
                break
            }
            if errno == EAGAIN || errno == EWOULDBLOCK {
                break
            }
            markExited(status: nil)
            break
        }
        if totalRead >= maxBytes || iterations >= maxIterations {
            outputQueue.async { [weak self] in
                self?.drainOutput()
            }
        }
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
            if self.directOutput {
                self.withRuntimeContext {
                    data.withUnsafeBytes { buffer in
                        guard let base = buffer.baseAddress else { return }
                        let ptr = base.assumingMemoryBound(to: CChar.self)
                        PSCALRuntimeSendInputForSession(self.sessionId, ptr, buffer.count)
                    }
                }
                return
            }
            guard self.inputFd >= 0 else { return }
            data.withUnsafeBytes { buffer in
                guard let base = buffer.baseAddress else { return }
                _ = write(self.inputFd, base, buffer.count)
            }
        }
    }

    private func consumeOutput(buffer: UnsafePointer<CChar>, length: Int) {
        guard length > 0 else { return }
        let data = Data(bytes: buffer, count: length)
        outputQueue.async { [weak self] in
            guard let self else { return }
            self.htermController.enqueueOutput(data)
            self.terminalBuffer.append(data: data)
        }
    }

    private func launchShellSession(readFd: inout Int32, writeFd: inout Int32) -> Bool {
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
                PSCALRuntimeCreateShellSession(argc,
                                               base,
                                               sessionId,
                                               &readFd,
                                               &writeFd)
            }
        }
        return result == 0
    }

    private func applyWindowSize(fd: Int32, columns: Int, rows: Int) {
        if fd < 0 || columns <= 0 || rows <= 0 {
            return
        }
        var ws = winsize()
        memset(&ws, 0, MemoryLayout<winsize>.size)
        ws.ws_col = UInt16(columns)
        ws.ws_row = UInt16(rows)
        _ = ioctl(fd, TIOCSWINSZ, &ws)
    }

    private func makeNonBlocking(fd: Int32) {
        let flags = fcntl(fd, F_GETFL, 0)
        if flags >= 0 {
            _ = fcntl(fd, F_SETFL, flags | O_NONBLOCK)
        }
    }

    private func configurePtySlave(_ fd: Int32) {
        guard fd >= 0 else { return }
        var term = termios()
        if tcgetattr(fd, &term) != 0 {
            return
        }
        term.c_lflag |= tcflag_t(ICANON | ECHO | ECHOE | ECHOK | ECHONL)
        term.c_lflag &= ~tcflag_t(ECHOCTL | ECHOKE)
        term.c_iflag |= tcflag_t(ICRNL | IXON)
        #if os(iOS)
        term.c_iflag |= tcflag_t(IUTF8)
        #endif
        term.c_oflag |= tcflag_t(OPOST | ONLCR)
        term.c_cflag |= tcflag_t(CS8 | CREAD)
        setControlChar(&term, index: VMIN, value: 1)
        setControlChar(&term, index: VTIME, value: 0)
        tcsetattr(fd, TCSANOW, &term)
    }

    private func setControlChar(_ term: inout termios, index: Int32, value: cc_t) {
        withUnsafeMutablePointer(to: &term.c_cc) { ptr in
            ptr.withMemoryRebound(to: cc_t.self, capacity: Int(NCCS)) { ccPtr in
                ccPtr[Int(index)] = value
            }
        }
    }

    private func closeIfValid(_ fd: Int32) {
        if fd >= 0 {
            close(fd)
        }
    }

    private func usesDirectPtyOutput() -> Bool {
        guard let raw = getenv("PSCALI_PTY_OUTPUT_DIRECT") else { return true }
        let value = String(cString: raw)
        return value.isEmpty || value != "0"
    }
}
