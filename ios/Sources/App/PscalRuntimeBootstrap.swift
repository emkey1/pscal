import Combine
import Foundation
import Darwin
#if canImport(pscal_terminal_host)
import pscal_terminal_host
#endif
import CoreLocation
import UIKit

@_cdecl("PSCALRuntimeOnProcessGroupEmpty")
func PSCALRuntimeOnProcessGroupEmpty(_ pgid: Int32) {
    DispatchQueue.main.async {
        TerminalTabManager.shared.closeTabForPgid(Int(pgid))
    }
}

@_silgen_name("pscalTtyCurrentPgid")
private func c_pscalTtyCurrentPgid() -> Int32

// MARK: - C Bridge Helpers

private func withCStringPointerRuntime<T>(_ string: String, _ body: (UnsafePointer<CChar>) -> T) -> T? {
    let utf8 = string.utf8CString
    return utf8.withUnsafeBufferPointer { buffer in
        guard let base = buffer.baseAddress else { return nil }
        return body(base)
    }
}

func traceLog(_ msg: String) {
    pscalRuntimeDebugLogBridge("[BRIDGE-TRACE] \(msg)")
}

private let runtimeLogMirrorsToConsole: Bool = {
    guard let value = ProcessInfo.processInfo.environment["PSCALI_RUNTIME_STDERR"] else {
        return false
    }
    return value != "0"
}()

private let runtimeDebugMirrorEnabled: Bool = {
    guard let value = ProcessInfo.processInfo.environment["PSCALI_DEBUG_MIRROR_TERMINAL"] else {
        return false
    }
    return value != "0"
}()

private func terminalLogURL() -> URL? {
    guard let raw = getenv("PSCALI_TERMINAL_LOG") else { return nil }
    let path = String(cString: raw)
    if path.isEmpty { return nil }
    let url: URL
    if path.hasPrefix("/") {
        url = URL(fileURLWithPath: path)
    } else {
        let base = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask).first
            ?? URL(fileURLWithPath: NSTemporaryDirectory(), isDirectory: true)
        let logDir = base.appendingPathComponent("var", isDirectory: true).appendingPathComponent("log", isDirectory: true)
        url = logDir.appendingPathComponent(path)
    }
    let dir = url.deletingLastPathComponent()
    try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
    return url
}

private func terminalLog(_ message: String) {
    guard let url = terminalLogURL() else { return }
    let line = message + "\n"
    guard let data = line.data(using: .utf8) else { return }
    if FileManager.default.fileExists(atPath: url.path) {
        if let handle = try? FileHandle(forWritingTo: url) {
            handle.seekToEndOfFile()
            handle.write(data)
            try? handle.close()
        }
    } else {
        try? data.write(to: url, options: .atomic)
    }
}

private let tabInitDebugEnabled: Bool = {
    guard let value = ProcessInfo.processInfo.environment["PSCALI_TAB_INIT_DEBUG"] else {
        return false
    }
    return value != "0"
}()

func tabInitLog(_ message: String) {
    guard tabInitDebugEnabled else { return }
    runtimeDebugLog("[TabInit] \(message)")
}

private let editorDebugLoggingEnabled: Bool = {
    guard let value = ProcessInfo.processInfo.environment["PSCALI_DEBUG_EDITOR"] else {
        return false
    }
    return value != "0"
}()

func runtimeDebugLog(_ message: String) {
    appendRuntimeDebugLog(message)
}

public final class RuntimeLogger {
    static let runtime = RuntimeLogger(filename: "pscal_runtime.log")

    private let queue: DispatchQueue
    private let fileURL: URL
    private static func timestampString() -> String {
        var ts = timespec()
        if clock_gettime(CLOCK_REALTIME, &ts) != 0 {
            return "0.000"
        }
        let seconds = Int64(ts.tv_sec)
        let millis = Int(ts.tv_nsec / 1_000_000)
        var millisString = String(millis)
        if millisString.count < 3 {
            millisString = String(repeating: "0", count: 3 - millisString.count) + millisString
        }
        return "\(seconds).\(millisString)"
    }

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

    private var sessionLines: [String] = []
    private var sessionBytes: Int = 0
    private let sessionByteLimit: Int = 512 * 1024

    private func recordSessionLine(_ line: String) {
        let delta = line.utf8.count + 1; // account for newline
        sessionLines.append(line)
        sessionBytes += delta
        if sessionByteLimit > 0 {
            while sessionBytes > sessionByteLimit && !sessionLines.isEmpty {
                let removed = sessionLines.removeFirst()
                sessionBytes -= (removed.utf8.count + 1)
            }
        }
    }

    public func resetSession() {
        queue.sync {
            sessionLines.removeAll()
            sessionBytes = 0
        }
    }

    private func sessionSnapshot() -> String {
        var snapshot = ""
        queue.sync {
            snapshot = sessionLines.joined(separator: "\n")
            if !snapshot.isEmpty {
                snapshot.append("\n")
            }
        }
        return snapshot
    }

    func copySessionSnapshotCString() -> UnsafeMutablePointer<CChar>? {
        let snapshot = sessionSnapshot()
        return withCStringPointerRuntime(snapshot) { strdup($0) }
    }

    public func append(_ message: String) {
        let timestamp = RuntimeLogger.timestampString()
        let line = "[\(timestamp)] \(message)"
        let record = line + "\n"
        guard let data = record.data(using: .utf8) else { return }
        queue.async {
            self.recordSessionLine(line)
            let directory = self.fileURL.deletingLastPathComponent()
            let fileManager = FileManager.default
            if !fileManager.fileExists(atPath: directory.path) {
                try? fileManager.createDirectory(at: directory, withIntermediateDirectories: true)
            }
            data.withUnsafeBytes { buffer in
                guard let base = buffer.baseAddress else { return }
                let fd = open(self.fileURL.path, O_WRONLY | O_CREAT | O_APPEND, 0o644)
                if fd >= 0 {
                    _ = write(fd, base, buffer.count)
                    close(fd)
                }
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

@_cdecl("pscalRuntimeCopySessionLog")
func pscalRuntimeCopySessionLogBridge() -> UnsafeMutablePointer<CChar>? {
    return RuntimeLogger.runtime.copySessionSnapshotCString()
}

@_cdecl("pscalRuntimeResetSessionLog")
func pscalRuntimeResetSessionLogBridge() {
    RuntimeLogger.runtime.resetSession()
}

@_cdecl("PSCALRuntimeSetDebugLogMirroring")
func PSCALRuntimeSetDebugLogMirroring(_ enable: Int32) {
    // Debug logging is global/shared
    PscalRuntimeBootstrap.shared.setDebugLogMirroring(enable != 0)
}

@_cdecl("pscalRuntimeDebugLog")
func pscalRuntimeDebugLogBridge(_ message: UnsafePointer<CChar>?) {
    guard let message = message else { return }
    let msg = String(cString: message)
    appendRuntimeDebugLog(msg)
    /* Mirror to the Xcode console only when explicitly requested via
     * PSCALI_RUNTIME_STDERR; editor debug tracing should remain in the
     * runtime log file by default to avoid noisy console spam. */
    // Debug logging is global/shared
    PscalRuntimeBootstrap.shared.forwardDebugLogToTerminalIfEnabled(msg)
}

// MARK: - Runtime Bootstrap

final class PscalRuntimeBootstrap: ObservableObject {
    static let shared = PscalRuntimeBootstrap() // Main/Global instance
    
    // MARK: - Runtime ID Generation
    private static let runtimeIdLock = NSLock()
    private static var nextRuntimeIdValue: Int = 1
    private static func nextRuntimeId() -> Int {
        runtimeIdLock.lock()
        defer { runtimeIdLock.unlock() }
        let value = nextRuntimeIdValue
        nextRuntimeIdValue += 1
        return value
    }
    
    // MARK: - Instance Registry
    private struct WeakBootstrap {
        weak var value: PscalRuntimeBootstrap?
    }
    private static var instanceRegistry = [UnsafeRawPointer: WeakBootstrap]()
    private static let registryLock = NSLock()
    
    static var current: PscalRuntimeBootstrap? {
        // Attempt to find the specific instance associated with the active C thread context
        if let ctx = PSCALRuntimeGetCurrentRuntimeContext() {
            let key = UnsafeRawPointer(ctx)
            registryLock.lock()
            defer { registryLock.unlock() }
            if let weakRef = instanceRegistry[key] {
                return weakRef.value
            }
        }
        // Fallback to shared for legacy/global calls (e.g. logging)
        return shared
    }
    
    // MARK: - Properties
    
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

    @Published private(set) var screenText: NSAttributedString = NSAttributedString(string: "Launching exsh...")
    @Published private(set) var exitStatus: Int32?
    @Published private(set) var cursorInfo: TerminalCursorInfo?
    @Published private(set) var terminalBackgroundColor: UIColor = UIColor.systemBackground
    @Published private(set) var editorRenderToken: UInt64 = 0
    
    // Instance-owned Bridge for editor mode
    let editorBridge = EditorTerminalBridge()
    
    // Mouse State Publishing
    @Published private(set) var mouseMode: TerminalBuffer.MouseMode = .none
    @Published private(set) var mouseEncoding: TerminalBuffer.MouseEncoding = .normal
    @Published private(set) var htermReady: Bool = false
    
    // Unique ID for this runtime session
    let runtimeId: Int

    private var started = false
    private let stateQueue = DispatchQueue(label: "com.pscal.runtime.state")
    private let stateQueueKey = DispatchSpecificKey<UInt8>()
    private let launchQueue = DispatchQueue(label: "com.pscal.runtime.launch", qos: .userInitiated)
    private let outputDrainQueue = DispatchQueue(label: "com.pscal.runtime.output", qos: .utility)
    private let runtimeStackSizeBytes = 32 * 1024 * 1024
    private var handlerContext: UnsafeMutableRawPointer?
    private let terminalBuffer: TerminalBuffer
    private enum GeometrySource: Hashable {
        case main
        case editor
    }
    private let inputQueue = DispatchQueue(label: "com.pscal.runtime.input", qos: .userInitiated)
    private var geometryBySource: [GeometrySource: TerminalGeometryMetrics]
    private var activeGeometry: TerminalGeometryMetrics
    private var activeGeometrySource: GeometrySource = .main
    private var editorModeActive: Bool = false
    private var editorRefreshPending: Bool = false
    private var appearanceObserver: NSObjectProtocol?
    private var mirrorDebugToTerminal: Bool = false
    private var waitingForRestart: Bool = false
    private var skipRcNextStart: Bool = false
    private var promptKickPending: Bool = true
    private var forceRestartPending: Bool = false
    private var closeOnExit: Bool = false
    private var awaitingExitConfirmation: Bool = false
    private var tabId: UInt64 = 0
    private var sessionId: UInt64 = 0
    private var shellContext: UnsafeMutableRawPointer?
    private var runtimeContext: OpaquePointer?
    private var outputDrainTimer: DispatchSourceTimer?
    private let outputDrainInterval: TimeInterval = 0.001
    private let outputDrainMaxBytes: Int = 1024 * 1024
    private let outputHandlerGroup = DispatchGroup()
    private var htermController: HtermTerminalController?
    private var outputHtermController: HtermTerminalController?
    private var htermReadyFlag: Bool = false
    private var pendingHtermOutput: [Data] = []
    private var pendingHtermBytes: Int = 0
    private let pendingHtermMaxBytes: Int = 512 * 1024
    private var htermHistoryOutput: [Data] = []
    private var htermHistoryBytes: Int = 0
    private var htermHistoryHead: Int = 0
    private let htermHistoryMaxBytes: Int = 2 * 1024 * 1024
    private var htermHistoryNeedsReplay: Bool = true
    private var shellPgid: Int = 0
    
    // THROTTLING VARS
    private var renderQueued = false
    private var lastRenderTime: TimeInterval = 0
    // 0.016 = ~60 FPS. This prevents the UI thread from locking up during massive paste operations.
    private let minRenderInterval: TimeInterval = 0.016 

    fileprivate func withRuntimeContext<T>(_ body: () -> T) -> T {
        guard let runtimeContext else {
            return body()
        }
        let previous = PSCALRuntimeGetCurrentRuntimeContext()
        PSCALRuntimeSetCurrentRuntimeContext(runtimeContext)
        defer { PSCALRuntimeSetCurrentRuntimeContext(previous) }
        return body()
    }

    private func withState<T>(_ body: () -> T) -> T {
        if DispatchQueue.getSpecific(key: stateQueueKey) != nil {
            return body()
        }
        return stateQueue.sync(execute: body)
    }
    
    private lazy var outputHandler: PSCALRuntimeOutputHandler = { data, length, context in
        guard let context, let base = data else { return }
        let unmanaged = Unmanaged<PscalRuntimeBootstrap>.fromOpaque(context)
        let bootstrap = unmanaged.takeUnretainedValue()
        bootstrap.outputHandlerGroup.enter()
        bootstrap.consumeOutput(buffer: base, length: Int(length))
        bootstrap.outputHandlerGroup.leave()
    }

    private lazy var exitHandler: PSCALRuntimeExitHandler = { status, context in
        guard let context else { return }
        let bootstrap = Unmanaged<PscalRuntimeBootstrap>.fromOpaque(context).takeUnretainedValue()
        bootstrap.handleExit(status: status)
    }

    private func enqueueHtermOutput(_ data: Data) {
        recordHtermHistory(data)
        if let controller = outputHtermController, htermReadyFlag, !htermHistoryNeedsReplay {
            controller.enqueueOutput(data)
            return
        }
        if pendingHtermBytes >= pendingHtermMaxBytes {
            return
        }
        pendingHtermOutput.append(data)
        pendingHtermBytes += data.count
    }

    private func flushPendingHtermOutputIfReady() {
        guard htermReadyFlag, let controller = outputHtermController else { return }
        if htermHistoryNeedsReplay {
            replayHtermHistory(controller: controller)
            return
        }
        guard !pendingHtermOutput.isEmpty else { return }
        pendingHtermOutput.forEach { controller.enqueueOutput($0) }
        pendingHtermOutput.removeAll()
        pendingHtermBytes = 0
    }

    private func recordHtermHistory(_ data: Data) {
        guard htermHistoryMaxBytes > 0 else { return }
        if data.count >= htermHistoryMaxBytes {
            let tail = data.suffix(htermHistoryMaxBytes)
            htermHistoryOutput = [Data(tail)]
            htermHistoryBytes = tail.count
            htermHistoryHead = 0
            return
        }
        htermHistoryOutput.append(data)
        htermHistoryBytes += data.count
        guard htermHistoryBytes > htermHistoryMaxBytes else { return }
        while htermHistoryBytes > htermHistoryMaxBytes && htermHistoryHead < htermHistoryOutput.count {
            let removed = htermHistoryOutput[htermHistoryHead]
            htermHistoryBytes -= removed.count
            htermHistoryHead += 1
        }
        if htermHistoryHead > 64 && htermHistoryHead > htermHistoryOutput.count / 2 {
            htermHistoryOutput.removeFirst(htermHistoryHead)
            htermHistoryHead = 0
        }
    }

    private func replayHtermHistory(controller: HtermTerminalController) {
        guard htermHistoryNeedsReplay else { return }
        if htermHistoryHead >= htermHistoryOutput.count {
            htermHistoryOutput.removeAll()
            htermHistoryHead = 0
            htermHistoryBytes = 0
        }
        let replayCount = max(0, htermHistoryOutput.count - htermHistoryHead)
        if htermHistoryBytes > 0 {
            tabInitLog("runtime=\(runtimeId) replay history chunks=\(replayCount) bytes=\(htermHistoryBytes)")
        } else {
            tabInitLog("runtime=\(runtimeId) replay history empty")
        }
        if htermHistoryBytes > 0 {
            for idx in htermHistoryHead..<htermHistoryOutput.count {
                controller.enqueueOutput(htermHistoryOutput[idx])
            }
        }
        pendingHtermOutput.removeAll()
        pendingHtermBytes = 0
        htermHistoryNeedsReplay = false
    }

    private func resetHtermHistory() {
        htermHistoryOutput.removeAll()
        htermHistoryBytes = 0
        htermHistoryHead = 0
        pendingHtermOutput.removeAll()
        pendingHtermBytes = 0
    }

    init() {
        // 1. Initialize properties first
        self.runtimeId = Self.nextRuntimeId()
        let initialMetrics = PscalRuntimeBootstrap.defaultGeometryMetrics()
        let createdRuntimeContext = PSCALRuntimeCreateRuntimeContext()
        
        self.runtimeContext = createdRuntimeContext
        self.geometryBySource = [.main: initialMetrics, .editor: initialMetrics]
        self.activeGeometry = initialMetrics
        self.stateQueue.setSpecific(key: stateQueueKey, value: 1)
        
        // Note: Using local 'createdRuntimeContext' inside the closure avoids accessing 'self' before initialization
        self.terminalBuffer = TerminalBuffer(columns: initialMetrics.columns,
                                             rows: initialMetrics.rows,
                                             scrollback: 400,
                                             dsrResponder: { data in
            data.withUnsafeBytes { buffer in
                guard let base = buffer.baseAddress else { return }
                let pointer = base.assumingMemoryBound(to: CChar.self)
                if let ctx = createdRuntimeContext {
                    let previous = PSCALRuntimeGetCurrentRuntimeContext()
                    PSCALRuntimeSetCurrentRuntimeContext(ctx)
                    PSCALRuntimeSendInput(pointer, buffer.count)
                    PSCALRuntimeSetCurrentRuntimeContext(previous)
                } else {
                    PSCALRuntimeSendInput(pointer, buffer.count)
                }
            }
        })
        
        // 2. 'self' is now fully initialized. Safe to use 'self' below.
        
        if let ctx = createdRuntimeContext {
            PscalRuntimeBootstrap.registryLock.lock()
            PscalRuntimeBootstrap.instanceRegistry[UnsafeRawPointer(ctx)] = WeakBootstrap(value: self)
            PscalRuntimeBootstrap.registryLock.unlock()
        }
        
        runtimeDebugLog("[Geometry] bootstrap size columns=\(initialMetrics.columns) rows=\(initialMetrics.rows)")

        self.terminalBuffer.setResizeHandler { [weak self] columns, rows in
            self?.handleTerminalResizeRequest(columns: columns, rows: rows)
        }
        
        self.terminalBuffer.onMouseModeChange = { [weak self] mode, encoding in
            self?.mouseMode = mode
            self?.mouseEncoding = encoding
        }
        
        withRuntimeContext {
            PSCALRuntimeUpdateWindowSize(Int32(initialMetrics.columns), Int32(initialMetrics.rows))
        }
        appearanceObserver = NotificationCenter.default.addObserver(forName: TerminalFontSettings.appearanceDidChangeNotification,
                                                                    object: nil,
                                                                    queue: .main) { [weak self] _ in
            // Font change can leave the prompt visually stale; nudge terminal.
            self?.send(" ")
            self?.send("\u{7F}")
            self?.scheduleRender()
        }
    }

    deinit {
        // Unregister
        if let ctx = runtimeContext {
            PscalRuntimeBootstrap.registryLock.lock()
            PscalRuntimeBootstrap.instanceRegistry.removeValue(forKey: UnsafeRawPointer(ctx))
            PscalRuntimeBootstrap.registryLock.unlock()
            
            PSCALRuntimeDestroyRuntimeContext(ctx)
        }
        
        if let ctx = shellContext {
            // Cannot use withRuntimeContext here as it might be destroyed
             PSCALRuntimeDestroyShellContext(ctx)
        }
    }

    func ensureHtermController() -> HtermTerminalController {
        dispatchPrecondition(condition: .onQueue(.main))
        if let controller = htermController {
            tabInitLog("runtime=\(runtimeId) reuse hterm controller=\(controller.instanceId)")
            return controller
        }
        let controller = HtermTerminalController()
        tabInitLog("runtime=\(runtimeId) create hterm controller=\(controller.instanceId)")
        htermController = controller
        outputDrainQueue.sync { [weak self] in
            self?.outputHtermController = controller
            self?.htermHistoryNeedsReplay = true
        }
        return controller
    }

    func htermControllerIfCreated() -> HtermTerminalController? {
        dispatchPrecondition(condition: .onQueue(.main))
        return htermController
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

    private func purgeTransientState() {
        let fm = FileManager.default
        let docs = fm.urls(for: .documentDirectory, in: .userDomainMask).first
            ?? URL(fileURLWithPath: NSTemporaryDirectory(), isDirectory: true)
        let tmpDir = docs.appendingPathComponent("tmp", isDirectory: true)
        if fm.fileExists(atPath: tmpDir.path) {
            try? fm.removeItem(at: tmpDir)
        }
        try? fm.createDirectory(at: tmpDir, withIntermediateDirectories: true)
    }

    func start() {
        tabInitLog("runtime=\(runtimeId) start request thread=\(Thread.isMainThread ? "main" : "bg")")
        let shouldStart = stateQueue.sync { () -> Bool in
            guard !started else { return false }
            started = true
            return true
        }
        guard shouldStart else {
            tabInitLog("runtime=\(runtimeId) start skipped (already started)")
            return
        }
        tabInitLog("runtime=\(runtimeId) start begin")
        stateQueue.async { self.promptKickPending = true }
        outputDrainQueue.async { [weak self] in
            self?.resetHtermHistory()
        }
        DispatchQueue.main.async {
            self.terminalBuffer.reset()
            self.screenText = NSAttributedString(string: "Launching exsh...")
            self.exitStatus = nil
            self.terminalBackgroundColor = UIColor.systemBackground
            self.cursorInfo = nil
        }

        launchQueue.async { [weak self] in
            guard let self else { return }
            self.configureSanitizerEnv()

            RuntimeLogger.runtime.resetSession()
            self.purgeTransientState()
            RuntimeAssetInstaller.shared.prepareWorkspace()
            if let runnerPath = RuntimeAssetInstaller.shared.ensureToolRunnerExecutable() {
                setenv("PSCALI_TOOL_RUNNER_PATH", runnerPath, 1)
            }
            let containerRoot = (NSHomeDirectory() as NSString).standardizingPath
            setenv("PSCALI_CONTAINER_ROOT", containerRoot, 1)
            let workdir = (containerRoot as NSString).appendingPathComponent("Documents/home")
            setenv("PSCALI_WORKDIR", workdir, 1)
            if getenv("PSCALI_PTY_OUTPUT_DIRECT") == nil {
                setenv("PSCALI_PTY_OUTPUT_DIRECT", "1", 1)
            }
            let shouldSkipRc = self.stateQueue.sync { self.skipRcNextStart }
            if shouldSkipRc {
                setenv("EXSH_SKIP_RC", "1", 1)
            } else {
                unsetenv("EXSH_SKIP_RC")
            }
            if self.shellContext == nil {
                self.shellContext = PSCALRuntimeCreateShellContext()
            }
            if let ctx = self.shellContext {
                self.withRuntimeContext {
                    PSCALRuntimeSetShellContext(ctx, 0)
                }
            }

            DispatchQueue.main.async {
                LocationDeviceProvider.shared.start()
                self.editorBridge.deactivate()
            }

            self.handlerContext = Unmanaged.passRetained(self).toOpaque()
            self.withRuntimeContext {
                PSCALRuntimeSetOutputBufferingEnabled(1)
                PSCALRuntimeConfigureHandlers(self.outputHandler, self.exitHandler, self.handlerContext)
            }
            self.startOutputDrain()

            let args = ["exsh"]
            var cArgs: [UnsafeMutablePointer<CChar>?] = args.map { strdup($0) }
            let argc = Int32(cArgs.count)
            cArgs.append(nil)
            defer {
                cArgs.forEach { if let ptr = $0 { free(ptr) } }
            }
            cArgs.withUnsafeMutableBufferPointer { buffer in
                guard let base = buffer.baseAddress else { return }
                let stackSize: size_t = numericCast(self.runtimeStackSizeBytes)
                self.withRuntimeContext {
                    _ = PSCALRuntimeLaunchExshWithStackSize(argc, base, stackSize)
                    
                    // FIX: Ensure PTY knows the window size immediately after launch.
                    let metrics = self.withState { self.activeGeometry }
                    let cols = Int32(metrics.columns)
                    let rows = Int32(metrics.rows)
                    if cols > 0 && rows > 0 {
                        PSCALRuntimeUpdateWindowSize(cols, rows)
                    }

                    let sessionId = PSCALRuntimeCurrentSessionId()
                    if sessionId != 0 {
                        self.stateQueue.async { self.sessionId = sessionId }
                        DispatchQueue.main.async {
                            TerminalTabManager.shared.registerShellSession(tabId: self.tabId,
                                                                           sessionId: sessionId)
                        }
                    }
                    let pgid = Int(c_pscalTtyCurrentPgid())
                    if pgid > 0 {
                        self.stateQueue.async { self.shellPgid = pgid }
                        DispatchQueue.main.async {
                            TerminalTabManager.shared.registerShellPgid(pgid,
                                                                         tabId: self.tabId)
                        }
                    }
                }
            }

            DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) {
                // Always send a carriage return to force prompt rendering
                self.send(" ")
                self.send("\u{7F}")
            }
            if self.stateQueue.sync(execute: { self.skipRcNextStart }) {
                unsetenv("EXSH_SKIP_RC")
                self.stateQueue.async { self.skipRcNextStart = false }
            }
        }
    }

    private func configureSanitizerEnv() {
        let key = "ASAN_OPTIONS"
        var options = ProcessInfo.processInfo.environment[key] ?? ""
        func appendOption(_ opt: String) {
            if !options.isEmpty && !options.hasSuffix(" ") {
                options.append(" ")
            }
            options.append(opt)
        }
        if !options.contains("use_sigaltstack=0") {
            appendOption("use_sigaltstack=0")
        }
        if !options.contains("detect_stack_use_after_return=0") {
            appendOption("detect_stack_use_after_return=0")
        }
        setenv(key, options, 1)
    }

    func send(_ text: String) {
        let normalized = text.replacingOccurrences(of: "\r\n", with: "\n").replacingOccurrences(of: "\r", with: "\n")
        guard let data = normalized.data(using: .utf8), !data.isEmpty else { return }
        var exitConfirmInput: String?
        var consumedForRestart = false
        stateQueue.sync {
            if awaitingExitConfirmation {
                awaitingExitConfirmation = false
                exitConfirmInput = normalized
                return
            }
            if waitingForRestart {
                waitingForRestart = false
                consumedForRestart = true
                started = false
            }
        }
        if let exitConfirmInput {
            let trimmed = exitConfirmInput.trimmingCharacters(in: .whitespacesAndNewlines)
            let first = trimmed.lowercased().first
            if first == "y" {
                Task { @MainActor in
                    TerminalTabManager.shared.requestAppExit()
                }
            } else {
                DispatchQueue.main.async { self.start() }
            }
            return
        }
        if consumedForRestart {
            DispatchQueue.main.async { self.start() }
            return
        }
        if data.count == 1 {
            switch data.first {
            case 0x03: // Ctrl-C
                withRuntimeContext {
                    PSCALRuntimeSendSignal(SIGINT)
                }
            case 0x1a: // Ctrl-Z
                withRuntimeContext {
                    PSCALRuntimeSendSignal(SIGTSTP)
                }
            default:
                break
            }
        }
        if editorBridge.interceptInputIfNeeded(data: data) {
            return
        }
        echoLocallyIfNeeded(text)
        let bytes = [UInt8](data)
        inputQueue.async { [weak self] in
            guard let self else { return }
            self.withRuntimeContext {
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
                        usleep(2000)
                    }
                }
            }
        }
    }

    func updateTerminalSize(columns: Int, rows: Int) {
        runtimeDebugLog("[Geometry] main update request columns=\(columns) rows=\(rows)")
        
        updateGeometry(from: .main, columns: columns, rows: rows)
        
        // CRITICAL FIX: If full-screen mode (Editor) is active, force the new geometry
        // down to the C-layer immediately, otherwise rotation is ignored.
        if isEditorModeActive() {
            runtimeDebugLog("[Geometry] Propagating main resize to Editor state")
            updateGeometry(from: .editor, columns: columns, rows: rows)
            let metrics = TerminalGeometryMetrics(columns: columns, rows: rows)
            withState { activeGeometry = metrics }
            withRuntimeContext {
                PSCALRuntimeUpdateWindowSize(Int32(columns), Int32(rows))
            }
        }
    }

    func updateEditorWindowSize(columns: Int, rows: Int) {
        runtimeDebugLog("[Geometry] editor update request columns=\(columns) rows=\(rows)")
        updateGeometry(from: .editor, columns: columns, rows: rows)
    }

    func resetTerminalState() {
        outputDrainQueue.async { [weak self] in
            self?.resetHtermHistory()
        }
        let metrics = withState { activeGeometry }
        let cols = metrics.columns
        let rows = metrics.rows
        if cols > 0 && rows > 0 {
            withRuntimeContext {
                PSCALRuntimeUpdateWindowSize(Int32(cols), Int32(rows))
            }
            if editorBridge.isActive {
                editorBridge.reset(columns: cols, rows: rows)
            }
        }
        terminalBuffer.reset()
        DispatchQueue.main.async {
            self.screenText = NSAttributedString(string: "")
            self.cursorInfo = nil
        }
        runtimeDebugLog("[Runtime] terminal reset invoked")
        send("\u{1B}c") // RIS
    }

    func forceExshRestart() {
        runtimeDebugLog("[Runtime] manual exsh restart requested from UI")
        stateQueue.async {
            self.waitingForRestart = false
            self.started = false
            self.skipRcNextStart = true
            self.forceRestartPending = true
        }
        withRuntimeContext {
            PSCALRuntimeSendSignal(SIGTERM)
            PSCALRuntimeSendSignal(SIGINT)
        }
        // Safety net: if the exit handler doesn't fire promptly, restart anyway.
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.35) { [weak self] in
            guard let self = self else { return }
            let stillPending = self.stateQueue.sync { self.forceRestartPending }
            if stillPending {
                self.stateQueue.async { self.forceRestartPending = false }
                self.start()
            }
        }
    }

    func requestClose() {
        withState { closeOnExit = true }
        send("\u{04}")
    }

    func assignTabId(_ id: UInt64) {
        stateQueue.async {
            self.tabId = id
        }
    }

    func currentShellPgid() -> Int {
        return stateQueue.sync { shellPgid }
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
        let outputLength = length
        let dataCopy = Data(bytesNoCopy: UnsafeMutableRawPointer(mutating: buffer),
                            count: length,
                            deallocator: .free)
        stateQueue.async { self.promptKickPending = false }
        outputDrainQueue.async { [weak self] in
            guard let self else { return }
            self.enqueueHtermOutput(dataCopy)
            self.appendOutput(dataCopy)
            self.withRuntimeContext {
                PSCALRuntimeOutputDidProcess(Int(outputLength))
            }
        }
    }

    func setDebugLogMirroring(_ enabled: Bool) {
        guard runtimeDebugMirrorEnabled else {
            stateQueue.async { self.mirrorDebugToTerminal = false }
            return
        }
        stateQueue.async { self.mirrorDebugToTerminal = enabled }
    }

    func forwardDebugLogToTerminalIfEnabled(_ message: String) {
        guard runtimeDebugMirrorEnabled else { return }
        guard !message.isEmpty else { return }
        var shouldMirror = false
        stateQueue.sync { shouldMirror = self.mirrorDebugToTerminal }
        if !shouldMirror {
            return
        }
        // Drop GPS NMEA chatter that can appear as noise from /dev/location.
        if message.hasPrefix("$GP") {
            return;
        }
        let line = message.hasSuffix("\n") ? message : (message + "\n")
        guard let data = line.data(using: .utf8) else { return }
        outputDrainQueue.async { [weak self] in
            guard let self else { return }
            self.enqueueHtermOutput(data)
            self.appendOutput(data)
        }
    }

    private func handleExit(status: Int32) {
        runtimeDebugLog("[Runtime] exsh exit detected (status=\(status)); awaiting user to restart")
        let stackSymbols = Thread.callStackSymbols.joined(separator: "\n")
        RuntimeLogger.runtime.append("Call stack for exit status \(status):\n\(stackSymbols)\n")
        let pgid = self.stateQueue.sync { self.shellPgid }
        if pgid > 0 {
            DispatchQueue.main.async {
                TerminalTabManager.shared.unregisterShellPgid(pgid)
            }
            self.stateQueue.async { self.shellPgid = 0 }
        }
        releaseHandlerContext()
        stopOutputDrain()
        Task { @MainActor in
            let tabId = self.stateQueue.sync { self.tabId }
            let closeResult = TerminalTabManager.shared.closeShellTab(tabId: tabId,
                                                                     runtime: self,
                                                                     status: status)
            switch closeResult {
            case .closed:
                self.stateQueue.async { self.closeOnExit = false }
                return
            case .missing:
                if self !== PscalRuntimeBootstrap.shared {
                    self.stateQueue.async { self.closeOnExit = false }
                    return
                }
            case .root:
                break
            }

            self.stateQueue.async {
                self.closeOnExit = false
                self.started = false
                if self.forceRestartPending {
                    self.waitingForRestart = false
                    self.awaitingExitConfirmation = false
                } else {
                    self.waitingForRestart = false
                    self.awaitingExitConfirmation = true
                }
                self.skipRcNextStart = true
            }

            let forced = self.stateQueue.sync { self.forceRestartPending }
            self.exitStatus = status
            self.setEditorModeActive(false)
            if forced {
                self.stateQueue.async { self.forceRestartPending = false }
                self.start()
            } else {
                let banner = "\r\nThis will exit the app. Are you sure? [y/N]\r\n"
                if let data = banner.data(using: .utf8) {
                    self.terminalBuffer.append(data: data) {
                        self.scheduleRender()
                    }
                }
            }
        }
    }

    private func releaseHandlerContext() {
        guard let context = handlerContext else { return }
        withRuntimeContext {
            PSCALRuntimeConfigureHandlers(nil, nil, nil)
        }
        outputHandlerGroup.notify(queue: outputDrainQueue) { [context] in
            Unmanaged<PscalRuntimeBootstrap>.fromOpaque(context).release()
        }
        handlerContext = nil
    }

    private func handleTerminalResizeRequest(columns: Int, rows: Int) {
        let metrics = withState { activeGeometry }
        runtimeDebugLog("[Geometry] remote resize request columns=\(columns) rows=\(rows); reasserting active \(metrics.columns)x\(metrics.rows)")
        DispatchQueue.main.async {
            self.refreshActiveGeometry(forceRuntimeUpdate: true)
        }
    }

    func attachHtermController(_ controller: HtermTerminalController?) {
        outputDrainQueue.async {
            guard let controller else {
                tabInitLog("runtime=\(self.runtimeId) attach nil controller")
                if Self.htermDebugEnabled {
                    NSLog("Hterm: attach main runtime (nil controller)")
                }
                return
            }
            if self.outputHtermController == nil {
                self.outputHtermController = controller
                self.htermHistoryNeedsReplay = true
                tabInitLog("runtime=\(self.runtimeId) attach set output controller=\(controller.instanceId)")
            }
            guard controller === self.outputHtermController else {
                let expectedId = self.outputHtermController?.instanceId ?? -1
                tabInitLog("runtime=\(self.runtimeId) attach ignored controller=\(controller.instanceId) expected=\(expectedId)")
                if Self.htermDebugEnabled {
                    NSLog("Hterm[%d]: attach main runtime ignored (unexpected controller)", controller.instanceId)
                }
                return
            }
            tabInitLog("runtime=\(self.runtimeId) attach controller=\(controller.instanceId) loaded=\(controller.isLoaded)")
            if Self.htermDebugEnabled {
                NSLog("Hterm[%d]: attach main runtime (loaded=%@)", controller.instanceId, controller.isLoaded.description)
            }
            self.htermReadyFlag = controller.isLoaded
            self.flushPendingHtermOutputIfReady()
        }
        DispatchQueue.main.async {
            self.htermReady = controller?.isLoaded ?? false
        }
    }

    func detachHtermController(_ controller: HtermTerminalController) {
        outputDrainQueue.async {
            guard controller === self.outputHtermController else {
                tabInitLog("runtime=\(self.runtimeId) detach ignored controller=\(controller.instanceId)")
                return
            }
            tabInitLog("runtime=\(self.runtimeId) detach controller=\(controller.instanceId)")
            if Self.htermDebugEnabled {
                NSLog("Hterm[%d]: detach main runtime", controller.instanceId)
            }
            if self.htermReadyFlag {
                self.htermReadyFlag = false
                self.scheduleRender()
            }
        }
        DispatchQueue.main.async {
            self.htermReady = false
        }
    }

    func markHtermLoaded() {
        outputDrainQueue.async {
            if self.htermReadyFlag {
                return
            }
            self.htermReadyFlag = true
            tabInitLog("runtime=\(self.runtimeId) mark hterm loaded")
            self.flushPendingHtermOutputIfReady()
        }
        DispatchQueue.main.async {
            self.htermReady = true
        }
    }

    func markHtermUnloaded() {
        outputDrainQueue.async {
            if !self.htermReadyFlag {
                self.htermHistoryNeedsReplay = true
                tabInitLog("runtime=\(self.runtimeId) mark hterm unloaded (already not ready)")
                return
            }
            self.htermReadyFlag = false
            self.htermHistoryNeedsReplay = true
            tabInitLog("runtime=\(self.runtimeId) mark hterm unloaded")
            self.scheduleRender()
        }
        DispatchQueue.main.async {
            self.htermReady = false
        }
    }

    private func appendOutput(_ data: Data) {
        if htermReadyFlag {
            terminalBuffer.append(data: data)
            return
        }
        terminalBuffer.append(data: data) { [weak self] in
            self?.scheduleRender()
        }
    }

    private func startOutputDrain() {
        outputDrainQueue.async { [weak self] in
            guard let self else { return }
            if self.outputDrainTimer != nil {
                return
            }
            let timer = DispatchSource.makeTimerSource(queue: self.outputDrainQueue)
            timer.schedule(deadline: .now(), repeating: self.outputDrainInterval, leeway: .milliseconds(2))
            timer.setEventHandler { [weak self] in
                self?.drainOutputOnce()
            }
            self.outputDrainTimer = timer
            timer.resume()
        }
    }

    private func stopOutputDrain() {
        outputDrainQueue.async { [weak self] in
            guard let self else { return }
            if let timer = self.outputDrainTimer {
                timer.cancel()
                self.outputDrainTimer = nil
            }
        }
    }

    private func drainOutputOnce() {
        var outPtr: UnsafeMutablePointer<UInt8>?
        let count = withRuntimeContext {
            PSCALRuntimeDrainOutput(&outPtr, outputDrainMaxBytes)
        }
        guard count > 0, let base = outPtr else { return }
        let data = Data(bytesNoCopy: base, count: Int(count), deallocator: .free)
        enqueueHtermOutput(data)
        appendOutput(data)
    }

    // --- RENDER SCHEDULING (THROTTLED) ---
    private func scheduleRender(preserveBackground: Bool = false) {
        if isEditorModeActive() {
            refreshEditorDisplay()
            return
        }
        if htermReadyFlag {
            return
        }
        
        // If a render is already pending, do nothing (coalesce updates)
        if renderQueued {
            return
        }
        
        let now = Date().timeIntervalSince1970
        let timeSinceLast = now - lastRenderTime
        
        // If we are under the throttle limit, delay the render.
        // This ensures massive I/O bursts (like pasting) don't lock the UI thread.
        if timeSinceLast < minRenderInterval {
            renderQueued = true
            let delay = minRenderInterval - timeSinceLast
            DispatchQueue.main.asyncAfter(deadline: .now() + delay) { [weak self] in
                self?.performRender(preserveBackground: preserveBackground)
            }
        } else {
            // Otherwise, render on the next runloop cycle
            renderQueued = true
            DispatchQueue.main.async { [weak self] in
                self?.performRender(preserveBackground: preserveBackground)
            }
        }
    }
    
    private func performRender(preserveBackground: Bool) {
        // Reset flags
        renderQueued = false
        lastRenderTime = Date().timeIntervalSince1970
        
        // Actual expensive rendering logic
        let allowScrollback = !self.isEditorModeActive()
        let snapshot = self.terminalBuffer.snapshot(includeScrollback: allowScrollback)
        let renderResult = PscalRuntimeBootstrap.renderJoined(snapshot: snapshot)
        let backgroundColor = snapshot.defaultBackground
        self.screenText = renderResult.text
        self.cursorInfo = renderResult.cursor
        if !preserveBackground {
            self.terminalBackgroundColor = backgroundColor
        }
    }
    // -------------------------------------

    func isEditorModeActive() -> Bool {
        return withState { editorModeActive }
    }

    func setEditorModeActive(_ active: Bool) {
        var metricsToApply: TerminalGeometryMetrics?
        withState {
            editorModeActive = active
            if active {
                let validMetrics = geometryBySource[.main] ?? activeGeometry
                activeGeometry = validMetrics
                activeGeometrySource = .main
                metricsToApply = validMetrics
            }
        }

        if active {
            if let validMetrics = metricsToApply {
                runtimeDebugLog("[Geometry] Forcing C-Runtime to sync with Main UI: \(validMetrics.columns)x\(validMetrics.rows)")
                withRuntimeContext {
                    PSCALRuntimeUpdateWindowSize(Int32(validMetrics.columns), Int32(validMetrics.rows))
                }
            }
            refreshEditorDisplay()
        } else {
            refreshActiveGeometry(forceRuntimeUpdate: true)
            scheduleRender()
        }
    }

    func refreshEditorDisplay() {
        guard editorBridge.isActive else { return }
        let shouldSchedule: Bool = stateQueue.sync {
            if editorRefreshPending {
                return false
            }
            editorRefreshPending = true
            return true
        }
        guard shouldSchedule else { return }
        DispatchQueue.main.async {
            self.terminalBackgroundColor = TerminalFontSettings.shared.backgroundColor
            self.editorRenderToken &+= 1
            EditorWindowManager.shared.refreshWindow()
            self.stateQueue.sync {
                self.editorRefreshPending = false
            }
        }
    }


    private func shouldEchoLocally() -> Bool {
        if isEditorModeActive() {
            return false
        }
        return false
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
        withState {
            geometryBySource[source] = metrics
        }
        refreshActiveGeometry()
    }

    private func refreshActiveGeometry(forceRuntimeUpdate: Bool = false) {
        var metricsToApply: TerminalGeometryMetrics?
        var source: GeometrySource = .main
        var geometryChanged = false
        var shouldResize = false
        var shouldApply = false
        withState {
            let desiredSource = determineDesiredGeometrySource()
            let desiredMetrics = geometryBySource[desiredSource] ?? geometryBySource[.main] ?? activeGeometry
            let changed = desiredMetrics != activeGeometry || desiredSource != activeGeometrySource
            if changed {
                activeGeometry = desiredMetrics
                activeGeometrySource = desiredSource
                metricsToApply = desiredMetrics
                source = desiredSource
                geometryChanged = true
                shouldResize = (desiredSource == .main)
                shouldApply = true
            } else if forceRuntimeUpdate {
                activeGeometrySource = desiredSource
                metricsToApply = desiredMetrics
                source = desiredSource
                shouldApply = true
            } else {
                activeGeometrySource = desiredSource
            }
        }
        guard shouldApply, let metrics = metricsToApply else {
            return
        }
        let sourceLabel: String = (source == .main) ? "main" : "editor"
        if geometryChanged {
            runtimeDebugLog("[Geometry] switching to \(sourceLabel) columns=\(metrics.columns) rows=\(metrics.rows)")
        } else if forceRuntimeUpdate {
            runtimeDebugLog("[Geometry] refreshing existing geometry source=\(sourceLabel) columns=\(metrics.columns) rows=\(metrics.rows)")
        }
        applyActiveGeometry(metrics: metrics, resizeTerminalBuffer: shouldResize, forceRuntimeUpdate: true)
    }

    private func determineDesiredGeometrySource() -> GeometrySource {
        if isEditorModeActive() && EditorWindowManager.shared.isVisible {
            return .editor
        }
        return .main
    }

    private func applyActiveGeometry(metrics: TerminalGeometryMetrics,
                                     resizeTerminalBuffer: Bool,
                                     forceRuntimeUpdate: Bool) {
        let runtimeColumns = metrics.columns
        let runtimeRows = metrics.rows
        if resizeTerminalBuffer {
            let resized = terminalBuffer.resize(columns: runtimeColumns, rows: runtimeRows)
            if resized {
                scheduleRender()
            }
        }
        if forceRuntimeUpdate || resizeTerminalBuffer {
            runtimeDebugLog("[Geometry] applying runtime window size columns=\(runtimeColumns) rows=\(runtimeRows) resizeBuffer=\(resizeTerminalBuffer) force=\(forceRuntimeUpdate)")
            withRuntimeContext {
                PSCALRuntimeUpdateWindowSize(Int32(runtimeColumns), Int32(runtimeRows))
            }
        }
    }

    static func defaultGeometryMetrics() -> TerminalGeometryMetrics {
        let font = TerminalFontSettings.shared.currentFont

        // Use the same logic the UI uses, just without a status row yet.
        let fallback = TerminalGeometryCalculator.fallbackMetrics(
            showingStatus: false,
            font: font
        )
        if fallback.columns > 0 && fallback.rows > 0 {
            return fallback
        }

        // Extremely defensive last-resort fallback – this should basically never run,
        // but it keeps us safe if something goes weird very early in app launch.
        let size: CGSize
        if let windowScene = UIApplication.shared.connectedScenes.first as? UIWindowScene,
           let window = windowScene.windows.first {
            size = window.bounds.size
        } else {
            size = UIScreen.main.bounds.size
        }

        // NOTE: characterMetrics now returns a CharacterMetrics struct.
        let charMetrics = TerminalGeometryCalculator.characterMetrics(for: font)
        let safeCharWidth = max(charMetrics.width, 1.0)
        let safeLineHeight = max(charMetrics.lineHeight, 1.0)

        let columns = max(20, min(Int(floor(size.width / safeCharWidth)), 2000))
        let rows    = max(23,  min(Int(floor(size.height / safeLineHeight)), 2000))

        return TerminalGeometryMetrics(columns: columns, rows: rows)
    }
    
    static func renderJoined(snapshot: TerminalBuffer.TerminalSnapshot) -> (text: NSAttributedString, cursor: TerminalCursorInfo?) {
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

struct EditorSnapshot {
    let text: String
    let attributedText: NSAttributedString
    let cursor: TerminalCursorInfo?
}

struct CellAttributes: Equatable {
    let fg: Int?
    let bg: Int?
    let bold: Bool
    let underline: Bool
    let inverse: Bool

    static let defaultAttributes = CellAttributes(fg: nil, bg: nil, bold: false, underline: false, inverse: false)
}

private struct TerminalPalette {
    static let shared = TerminalPalette()

    private static func buildPalette() -> [UIColor] {
        var palette: [UIColor] = []
        // 0-15 standard/bright
        let base: [UIColor] = [
            UIColor(red: 0/255.0, green: 0/255.0, blue: 0/255.0, alpha: 1),
            UIColor(red: 205/255.0, green: 0/255.0, blue: 0/255.0, alpha: 1),
            UIColor(red: 0/255.0, green: 205/255.0, blue: 0/255.0, alpha: 1),
            UIColor(red: 205/255.0, green: 205/255.0, blue: 0/255.0, alpha: 1),
            UIColor(red: 0/255.0, green: 0/255.0, blue: 238/255.0, alpha: 1),
            UIColor(red: 205/255.0, green: 0/255.0, blue: 205/255.0, alpha: 1),
            UIColor(red: 0/255.0, green: 205/255.0, blue: 205/255.0, alpha: 1),
            UIColor(red: 229/255.0, green: 229/255.0, blue: 229/255.0, alpha: 1),
            UIColor(red: 127/255.0, green: 127/255.0, blue: 127/255.0, alpha: 1),
            UIColor(red: 255/255.0, green: 0/255.0, blue: 0/255.0, alpha: 1),
            UIColor(red: 0/255.0, green: 255/255.0, blue: 0/255.0, alpha: 1),
            UIColor(red: 255/255.0, green: 255/255.0, blue: 0/255.0, alpha: 1),
            UIColor(red: 92/255.0, green: 92/255.0, blue: 255/255.0, alpha: 1),
            UIColor(red: 255/255.0, green: 0/255.0, blue: 255/255.0, alpha: 1),
            UIColor(red: 0/255.0, green: 255/255.0, blue: 255/255.0, alpha: 1),
            UIColor(red: 255/255.0, green: 255/255.0, blue: 255/255.0, alpha: 1)
        ]
        palette.append(contentsOf: base)
        // 16-231 color cube
        let steps: [CGFloat] = [0, 95, 135, 175, 215, 255]
        for r in 0..<6 {
            for g in 0..<6 {
                for b in 0..<6 {
                    let color = UIColor(red: steps[r]/255.0,
                                        green: steps[g]/255.0,
                                        blue: steps[b]/255.0,
                                        alpha: 1)
                    palette.append(color)
                }
            }
        }
        // 232-255 grayscale
        for i in 0..<24 {
            let val = CGFloat(8 + i * 10)
            let c = val/255.0
            palette.append(UIColor(red: c, green: c, blue: c, alpha: 1))
        }
        return palette
    }

    private let colors: [UIColor] = TerminalPalette.buildPalette()

    func resolve(attr: CellAttributes, defaultFG: UIColor, defaultBG: UIColor) -> (fg: UIColor, bg: UIColor) {
        let fgColor = color(for: attr.fg, default: defaultFG)
        let bgColor = color(for: attr.bg, default: defaultBG)
        if attr.inverse {
            return (bgColor, fgColor)
        }
        return (fgColor, bgColor)
    }

    private func color(for index: Int?, default: UIColor) -> UIColor {
        guard let idx = index else { return `default` }
        if idx >= 0 && idx < colors.count {
            return colors[idx]
        }
        return `default`
    }
}

final class EditorTerminalBridge {
    // REMOVED static shared instance to support multiple tabs
    // This class is now owned by specific PscalRuntimeBootstrap instances.

    private struct ScreenState {
        var rows: Int = 0
        var columns: Int = 0
        var grid: [[Character]] = []
        var attrs: [[CellAttributes]] = []
        var cursorRow: Int = 0
        var cursorCol: Int = 0
        var active: Bool = false
        var cursorVisible: Bool = true
    }

    private let stateQueue = DispatchQueue(label: "com.pscal.editor.bridge.state", attributes: .concurrent)
    private var state = ScreenState()
    private let inputCondition = NSCondition()
    private var pendingInput: [UInt8] = []
    private var altScreenState: ScreenState?

    var isActive: Bool {
        stateQueue.sync { state.active }
    }

    func activate(columns: Int, rows: Int) {
        stateQueue.sync(flags: .barrier) {
            state.active = true
            rebuildState(columns: columns, rows: rows)
        }
        inputCondition.lock()
        pendingInput.removeAll()
        inputCondition.unlock()
    }

    func reset(columns: Int, rows: Int) {
        stateQueue.sync(flags: .barrier) {
            guard state.active else { return }
            rebuildState(columns: columns, rows: rows)
        }
        inputCondition.lock()
        pendingInput.removeAll()
        inputCondition.unlock()
    }

    func deactivate() {
        stateQueue.sync(flags: .barrier) {
            state.active = false
            state.grid.removeAll()
            state.attrs.removeAll()
            state.rows = 0
            state.columns = 0
            state.cursorRow = 0
            state.cursorCol = 0
            state.cursorVisible = true
            altScreenState = nil
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
            let blankAttr = Array(repeating: CellAttributes.defaultAttributes, count: newCols)
            var newGrid = Array(repeating: blankRow, count: newRows)
            var newAttrs = Array(repeating: blankAttr, count: newRows)
            let copyRows = min(newRows, state.rows)
            let copyCols = min(newCols, state.columns)
            for r in 0..<copyRows {
                for c in 0..<copyCols {
                    newGrid[r][c] = state.grid[r][c]
                    newAttrs[r][c] = state.attrs[r][c]
                }
            }
            state.grid = newGrid
            state.attrs = newAttrs
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
            let blankAttr = Array(repeating: CellAttributes.defaultAttributes, count: state.columns)
            state.grid = Array(repeating: blankRow, count: state.rows)
            state.attrs = Array(repeating: blankAttr, count: state.rows)
            state.cursorRow = 0
            state.cursorCol = 0
        }
    }

    private func rebuildState(columns: Int, rows: Int) {
        state.columns = max(1, columns)
        state.rows = max(1, rows)
        let blankRow = Array(repeating: Character(" "), count: state.columns)
        let blankAttr = Array(repeating: CellAttributes.defaultAttributes, count: state.columns)
        state.grid = Array(repeating: blankRow, count: state.rows)
        state.attrs = Array(repeating: blankAttr, count: state.rows)
        state.cursorRow = 0
        state.cursorCol = 0
        state.cursorVisible = true
        altScreenState = nil
    }

    private func clearLineSegment(row: Int, start: Int, end: Int) {
        guard state.active,
              row >= 0, row < state.rows,
              start < end else { return }
        let begin = max(0, min(start, state.columns))
        let stop = max(begin, min(end, state.columns))
        let space = Character(" ")
        for col in begin..<stop {
            state.grid[row][col] = space
            state.attrs[row][col] = CellAttributes.defaultAttributes
        }
    }

    func clearToEndOfScreen(row: Int, col: Int) {
        stateQueue.sync(flags: .barrier) {
            guard state.active else { return }
            let r = max(0, min(row, state.rows - 1))
            let c = max(0, min(col, state.columns))
            clearLineSegment(row: r, start: c, end: state.columns)
            if r + 1 < state.rows {
                let blankRow = Array(repeating: Character(" "), count: state.columns)
                let blankAttr = Array(repeating: CellAttributes.defaultAttributes, count: state.columns)
                for rr in (r + 1)..<state.rows {
                    state.grid[rr] = blankRow
                    state.attrs[rr] = blankAttr
                }
            }
        }
    }

    func clearToStartOfScreen(row: Int, col: Int) {
        stateQueue.sync(flags: .barrier) {
            guard state.active else { return }
            let r = max(0, min(row, state.rows - 1))
            let c = max(0, min(col, state.columns))
            if r > 0 {
                let blankRow = Array(repeating: Character(" "), count: state.columns)
                let blankAttr = Array(repeating: CellAttributes.defaultAttributes, count: state.columns)
                for rr in 0..<r {
                    state.grid[rr] = blankRow
                    state.attrs[rr] = blankAttr
                }
            }
            clearLineSegment(row: r, start: 0, end: c + 1)
        }
    }

    func clearLineFromCursor(row: Int, col: Int) {
        stateQueue.sync(flags: .barrier) {
            clearLineSegment(row: row, start: col, end: state.columns)
        }
    }

    func clearLineToCursor(row: Int, col: Int) {
        stateQueue.sync(flags: .barrier) {
            clearLineSegment(row: row, start: 0, end: col + 1)
        }
    }

    func clearLine(row: Int) {
        stateQueue.sync(flags: .barrier) {
            guard state.active, row >= 0, row < state.rows else { return }
            let blankRow = Array(repeating: Character(" "), count: state.columns)
            let blankAttr = Array(repeating: CellAttributes.defaultAttributes, count: state.columns)
            state.grid[row] = blankRow
            state.attrs[row] = blankAttr
        }
    }

    func insertLines(at row: Int, count: Int) {
        stateQueue.sync(flags: .barrier) {
            guard state.active, state.rows > 0, state.columns > 0 else { return }
            let targetRow = max(0, min(row, state.rows - 1))
            let linesToInsert = min(max(1, count), state.rows - targetRow)
            let savedCursorRow = state.cursorRow
            let savedCursorCol = state.cursorCol
            let blankRow = Array(repeating: Character(" "), count: state.columns)
            let blankAttr = Array(repeating: CellAttributes.defaultAttributes, count: state.columns)
            for _ in 0..<linesToInsert {
                state.grid.insert(blankRow, at: targetRow)
                state.grid.removeLast()
                state.attrs.insert(blankAttr, at: targetRow)
                state.attrs.removeLast()
            }
            state.cursorRow = savedCursorRow
            state.cursorCol = savedCursorCol
        }
    }

    func deleteLines(at row: Int, count: Int) {
        stateQueue.sync(flags: .barrier) {
            guard state.active, state.rows > 0, state.columns > 0 else { return }
            let targetRow = max(0, min(row, state.rows - 1))
            let linesToDelete = min(max(1, count), state.rows - targetRow)
            let savedCursorRow = state.cursorRow
            let savedCursorCol = state.cursorCol
            let blankRow = Array(repeating: Character(" "), count: state.columns)
            for _ in 0..<linesToDelete {
                state.grid.remove(at: targetRow)
                state.grid.append(blankRow)
                state.attrs.remove(at: targetRow)
                state.attrs.append(Array(repeating: CellAttributes.defaultAttributes, count: state.columns))
            }
            state.cursorRow = savedCursorRow
            state.cursorCol = savedCursorCol
        }
    }

    func insertChars(at row: Int, col: Int, count: Int) {
        stateQueue.sync(flags: .barrier) {
            guard state.active, state.rows > 0, state.columns > 0 else { return }
            let r = max(0, min(row, state.rows - 1))
            let c = max(0, min(col, state.columns - 1))
            let amt = min(max(1, count), state.columns - c)
            var line = state.grid[r]
            var attrLine = state.attrs[r]
            for idx in stride(from: state.columns - 1, through: c + amt, by: -1) {
                line[idx] = line[idx - amt]
                attrLine[idx] = attrLine[idx - amt]
            }
            for idx in c..<(c + amt) {
                line[idx] = Character(" ")
                attrLine[idx] = CellAttributes.defaultAttributes
            }
            state.grid[r] = line
            state.attrs[r] = attrLine
        }
    }

    func deleteChars(at row: Int, col: Int, count: Int) {
        stateQueue.sync(flags: .barrier) {
            guard state.active, state.rows > 0, state.columns > 0 else { return }
            let r = max(0, min(row, state.rows - 1))
            let c = max(0, min(col, state.columns - 1))
            let amt = min(max(1, count), state.columns - c)
            var line = state.grid[r]
            var attrLine = state.attrs[r]
            for idx in c..<(state.columns - amt) {
                line[idx] = line[idx + amt]
                attrLine[idx] = attrLine[idx + amt]
            }
            for idx in (state.columns - amt)..<state.columns {
                line[idx] = Character(" ")
                attrLine[idx] = CellAttributes.defaultAttributes
            }
            state.grid[r] = line
            state.attrs[r] = attrLine
        }
    }

    func setCursorVisible(_ visible: Bool) {
        stateQueue.sync(flags: .barrier) {
            guard state.active else { return }
            state.cursorVisible = visible
        }
    }

    func enterAltScreen() {
        stateQueue.sync(flags: .barrier) {
            guard state.active else { return }
            altScreenState = state
            let blankRow = Array(repeating: Character(" "), count: state.columns)
            let blankAttr = Array(repeating: CellAttributes.defaultAttributes, count: state.columns)
            state.grid = Array(repeating: blankRow, count: state.rows)
            state.attrs = Array(repeating: blankAttr, count: state.rows)
            state.cursorRow = 0
            state.cursorCol = 0
        }
    }

    func exitAltScreen() {
        stateQueue.sync(flags: .barrier) {
            guard let saved = altScreenState else { return }
            state = saved
            altScreenState = nil
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
        if row < 0 { return 0 }
        if row >= state.rows { return state.rows - 1 }
        return row
    }

    func draw(row: Int, column: Int, text: UnsafePointer<CChar>?, length: Int, fg: Int32, bg: Int32, attr: Int32) {
        guard let text, length > 0 else { return }
        let data = Data(bytes: text, count: length)
        let string = String(decoding: data, as: UTF8.self)
        stateQueue.sync(flags: .barrier) {
            guard state.active, state.rows > 0, state.columns > 0 else { return }
            var targetRow = max(0, min(normalizeRow(row), state.rows - 1))
            var targetCol = max(0, min(column, state.columns - 1))
            let attributes = CellAttributes(fg: fg >= 0 ? Int(fg) : nil,
                                            bg: bg >= 0 ? Int(bg) : nil,
                                            bold: (attr & 1) != 0,
                                            underline: (attr & 2) != 0,
                                            inverse: (attr & 4) != 0)
            for ch in string {
                if ch == "\n" {
                    targetRow = min(state.rows - 1, targetRow + 1);
                    targetCol = 0;
                    continue
                }
                if ch == "\r" {
                    targetCol = 0
                    continue
                }
                if let scalar = ch.unicodeScalars.first,
                   scalar.properties.generalCategory == .control {
                    continue
                }
                if targetCol >= state.columns {
                    targetCol = state.columns - 1
                }
                state.grid[targetRow][targetCol] = ch
                state.attrs[targetRow][targetCol] = attributes
                targetCol += 1
            }
            state.cursorRow = targetRow
            state.cursorCol = min(targetCol, state.columns - 1)
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

    func snapshot() -> EditorSnapshot {
        let currentState = stateQueue.sync { state }
        let lines: [String]
        if currentState.rows == 0 || currentState.columns == 0 || currentState.grid.isEmpty {
            lines = [""]
        } else {
            lines = currentState.grid.map { String($0) }
        }
        let palette = TerminalPalette.shared
        let fgDefault = TerminalFontSettings.shared.foregroundColor
        let bgDefault = TerminalFontSettings.shared.backgroundColor
        let baseFont = TerminalFontSettings.shared.currentFont
        let boldFont: UIFont = {
            if let desc = baseFont.fontDescriptor.withSymbolicTraits(.traitBold) {
                return UIFont(descriptor: desc, size: baseFont.pointSize)
            }
            return baseFont
        }()

        var plainBuilder = String()
        plainBuilder.reserveCapacity(currentState.rows * currentState.columns + max(currentState.rows - 1, 0))
        let attributed = NSMutableAttributedString()
        let newlineAttrs: [NSAttributedString.Key: Any] = [.font: baseFont,
                                                           .foregroundColor: fgDefault,
                                                           .backgroundColor: bgDefault]

        for r in 0..<currentState.rows {
            for c in 0..<currentState.columns {
                let ch = currentState.grid[r][c]
                let attr = currentState.attrs[r][c]
                plainBuilder.append(ch)
                let resolved = palette.resolve(attr: attr, defaultFG: fgDefault, defaultBG: bgDefault)
                let attrs: [NSAttributedString.Key: Any] = [
                    .font: attr.bold ? boldFont : baseFont,
                    .foregroundColor: resolved.fg,
                    .backgroundColor: resolved.bg,
                    .underlineStyle: attr.underline ? NSUnderlineStyle.single.rawValue : 0
                ]
                attributed.append(NSAttributedString(string: String(ch), attributes: attrs))
            }
            if r < currentState.rows - 1 {
                plainBuilder.append("\n")
                attributed.append(NSAttributedString(string: "\n", attributes: newlineAttrs))
            }
        }
        let joined = plainBuilder
        let cursor: TerminalCursorInfo?
        if currentState.active && currentState.cursorVisible && !lines.isEmpty {
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
        return EditorSnapshot(text: joined,
                              attributedText: attributed,
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

// MARK: - C Bridge Callbacks (Routed to Correct Instance)

@_cdecl("pscalTerminalBegin")
func pscalTerminalBegin(_ columns: Int32, _ rows: Int32) {
    if editorDebugLoggingEnabled {
        runtimeDebugLog("pscalTerminalBegin cols=\(columns) rows=\(rows)")
    }
    // ROUTING: Use .current instance derived from C Context
    guard let bootstrap = PscalRuntimeBootstrap.current else { return }
    bootstrap.editorBridge.activate(columns: Int(columns), rows: Int(rows))
    bootstrap.setEditorModeActive(true)
#if EDITOR_FLOATING_WINDOW
    EditorWindowManager.shared.showWindow()
#endif
}

@_cdecl("pscalTerminalEnd")
func pscalTerminalEnd() {
    if editorDebugLoggingEnabled {
        runtimeDebugLog("pscalTerminalEnd")
    }
    guard let bootstrap = PscalRuntimeBootstrap.current else { return }
    bootstrap.editorBridge.deactivate()
    bootstrap.setEditorModeActive(false)
#if EDITOR_FLOATING_WINDOW
    EditorWindowManager.shared.hideWindow()
#endif
}

@_cdecl("pscalTerminalResize")
func pscalTerminalResize(_ columns: Int32, _ rows: Int32) {
    if editorDebugLoggingEnabled {
        runtimeDebugLog("pscalTerminalResize cols=\(columns) rows=\(rows)")
    }
    guard let bootstrap = PscalRuntimeBootstrap.current else { return }
    bootstrap.editorBridge.resize(columns: Int(columns), rows: Int(rows))
    bootstrap.refreshEditorDisplay()
}

@_cdecl("pscalTerminalRender")
func pscalTerminalRender(_ utf8: UnsafePointer<CChar>?, _ len: Int32, _ row: Int32, _ col: Int32, _ fg: Int64, _ bg: Int64, _ attr: Int32) {
    if terminalLogURL() != nil {
        let previewLen = min(Int(len), 8)
        let bytes = (0..<previewLen).compactMap { idx in utf8?[idx] }.map { String(format: "%02X", $0) }.joined(separator: " ")
        terminalLog("RENDER row=\(row) col=\(col) len=\(len) fg=\(fg) bg=\(bg) attr=\(attr) bytes=\(bytes)")
    }
    guard let bootstrap = PscalRuntimeBootstrap.current else { return }
    bootstrap.editorBridge.draw(row: Int(row),
                                     column: Int(col),
                                     text: utf8,
                                     length: Int(len),
                                     fg: Int32(truncatingIfNeeded: fg),
                                     bg: Int32(truncatingIfNeeded: bg),
                                     attr: attr)
    bootstrap.refreshEditorDisplay()
}

@_cdecl("pscalTerminalClear")
func pscalTerminalClear() {
    if editorDebugLoggingEnabled {
        runtimeDebugLog("pscalTerminalClear")
    }
    if terminalLogURL() != nil { terminalLog("CLEAR") }
    guard let bootstrap = PscalRuntimeBootstrap.current else { return }
    bootstrap.editorBridge.clear()
    bootstrap.refreshEditorDisplay()
}

@_cdecl("pscalTerminalMoveCursor")
func pscalTerminalMoveCursor(_ row: Int32, _ column: Int32) {
    if terminalLogURL() != nil {
        terminalLog("CURSOR row=\(row) col=\(column)")
    }
    guard let bootstrap = PscalRuntimeBootstrap.current else { return }
    bootstrap.editorBridge.moveCursor(row: Int(row), column: Int(column))
    bootstrap.refreshEditorDisplay()
}
@_cdecl("pscalTerminalClearEol")
func pscalTerminalClearEol(_ row: Int32, _ column: Int32) {
    guard let bootstrap = PscalRuntimeBootstrap.current else { return }
    bootstrap.editorBridge.clearToEndOfLine(row: Int(row), column: Int(column))
    bootstrap.refreshEditorDisplay()
}

@_cdecl("pscalTerminalClearBol")
func pscalTerminalClearBol(_ row: Int32, _ column: Int32) {
    guard let bootstrap = PscalRuntimeBootstrap.current else { return }
    bootstrap.editorBridge.clearLineToCursor(row: Int(row), col: Int(column))
    bootstrap.refreshEditorDisplay()
}

@_cdecl("pscalTerminalClearLine")
func pscalTerminalClearLine(_ row: Int32) {
    if terminalLogURL() != nil { terminalLog("CLEAR_LINE row=\(row)") }
    guard let bootstrap = PscalRuntimeBootstrap.current else { return }
    bootstrap.editorBridge.clearLine(row: Int(row))
    bootstrap.refreshEditorDisplay()
}

@_cdecl("pscalTerminalClearScreenFromCursor")
func pscalTerminalClearScreenFromCursor(_ row: Int32, _ column: Int32) {
    if terminalLogURL() != nil { terminalLog("CLEAR_SCR_FROM row=\(row) col=\(column)") }
    guard let bootstrap = PscalRuntimeBootstrap.current else { return }
    bootstrap.editorBridge.clearToEndOfScreen(row: Int(row), col: Int(column))
    bootstrap.refreshEditorDisplay()
}

@_cdecl("pscalTerminalClearScreenToCursor")
func pscalTerminalClearScreenToCursor(_ row: Int32, _ column: Int32) {
    if terminalLogURL() != nil { terminalLog("CLEAR_SCR_TO row=\(row) col=\(column)") }
    guard let bootstrap = PscalRuntimeBootstrap.current else { return }
    bootstrap.editorBridge.clearToStartOfScreen(row: Int(row), col: Int(column))
    bootstrap.refreshEditorDisplay()
}

@_cdecl("pscalTerminalInsertLines")
func pscalTerminalInsertLines(_ row: Int32, _ count: Int32) {
    if terminalLogURL() != nil { terminalLog("IL row=\(row) count=\(count)") }
    guard let bootstrap = PscalRuntimeBootstrap.current else { return }
    bootstrap.editorBridge.insertLines(at: Int(row), count: Int(max(1, count)))
    bootstrap.refreshEditorDisplay()
}

@_cdecl("pscalTerminalDeleteLines")
func pscalTerminalDeleteLines(_ row: Int32, _ count: Int32) {
    if terminalLogURL() != nil { terminalLog("DL row=\(row) count=\(count)") }
    guard let bootstrap = PscalRuntimeBootstrap.current else { return }
    bootstrap.editorBridge.deleteLines(at: Int(row), count: Int(max(1, count)))
    bootstrap.refreshEditorDisplay()
}

@_cdecl("pscalTerminalInsertChars")
func pscalTerminalInsertChars(_ row: Int32, _ col: Int32, _ count: Int32) {
    if terminalLogURL() != nil { terminalLog("ICH row=\(row) col=\(col) count=\(count)") }
    guard let bootstrap = PscalRuntimeBootstrap.current else { return }
    bootstrap.editorBridge.insertChars(at: Int(row), col: Int(col), count: Int(max(1, count)))
    bootstrap.refreshEditorDisplay()
}

@_cdecl("pscalTerminalDeleteChars")
func pscalTerminalDeleteChars(_ row: Int32, _ col: Int32, _ count: Int32) {
    if terminalLogURL() != nil { terminalLog("DCH row=\(row) col=\(col) count=\(count)") }
    guard let bootstrap = PscalRuntimeBootstrap.current else { return }
    bootstrap.editorBridge.deleteChars(at: Int(row), col: Int(col), count: Int(max(1, count)))
    bootstrap.refreshEditorDisplay()
}

@_cdecl("pscalTerminalEnterAltScreen")
func pscalTerminalEnterAltScreen() {
    if terminalLogURL() != nil { terminalLog("ALT_ENTER") }
    guard let bootstrap = PscalRuntimeBootstrap.current else { return }
    bootstrap.editorBridge.enterAltScreen()
    bootstrap.refreshEditorDisplay()
}

@_cdecl("pscalTerminalExitAltScreen")
func pscalTerminalExitAltScreen() {
    if terminalLogURL() != nil { terminalLog("ALT_EXIT") }
    guard let bootstrap = PscalRuntimeBootstrap.current else { return }
    bootstrap.editorBridge.exitAltScreen()
    bootstrap.refreshEditorDisplay()
}

@_cdecl("pscalTerminalSetCursorVisible")
func pscalTerminalSetCursorVisible(_ visible: Int32) {
    guard let bootstrap = PscalRuntimeBootstrap.current else { return }
    bootstrap.editorBridge.setCursorVisible(visible != 0)
    bootstrap.refreshEditorDisplay()
}

@_cdecl("pscalTerminalRead")
func pscalTerminalRead(_ buffer: UnsafeMutablePointer<UInt8>?, _ maxlen: Int32, _ timeout: Int32) -> Int32 {
    guard let buffer else { return 0 }
    // Read from the current bootstrap's bridge
    guard let bootstrap = PscalRuntimeBootstrap.current else { return 0 }
    let bytesRead = bootstrap.editorBridge.read(into: buffer, maxLength: Int(maxlen), timeoutMs: Int(timeout))
    return Int32(bytesRead)
}

@_cdecl("pscalEditorDump")
func pscalEditorDump() {
    // Debug dump logic for current instance
    guard let bootstrap = PscalRuntimeBootstrap.current else { return }
    let snapshot = bootstrap.editorBridge.snapshot()
    if let ptr = withCStringPointerRuntime(snapshot.text, { $0 }) {
        fputs(ptr, stdout)
        fputc(0x0A, stdout)
    }
    let debugState = bootstrap.editorBridge.debugState()
    if let cursor = snapshot.cursor {
        let cursorLine = "[editordump] cursor row=\(cursor.row) col=\(cursor.column)\n"
        if let ptr = withCStringPointerRuntime(cursorLine, { $0 }) {
            fputs(ptr, stderr)
        }
    } else {
        fputs("[editordump] cursor unavailable\n", stderr)
    }
    let stateLine = "[editordump] active=\(debugState.active) rows=\(debugState.rows) cols=\(debugState.columns)\n"
    if let ptr = withCStringPointerRuntime(stateLine, { $0 }) {
        fputs(ptr, stderr)
    }
}

@_cdecl("pscalElvisDump")
func pscalElvisDump() {
    pscalEditorDump()
}

final class LocationDeviceProvider: NSObject, CLLocationManagerDelegate {
    static let shared = LocationDeviceProvider()

    private let queue = DispatchQueue(label: "com.pscal.location.device")
    private let locationManager: CLLocationManager
    private let debugEnabled: Bool = {
        guard let raw = ProcessInfo.processInfo.environment["PSCALI_LOCATION_DEBUG"] else {
            return false
        }
        return !raw.isEmpty && raw != "0"
    }()
    private var started = false
    private var deviceEnabled = true
    private var locationActive = false
    private var latestLocation: CLLocation?
    private var lastLocationRequest: TimeInterval = 0
    private let locationRequestInterval: TimeInterval = 5.0
    private var requestTimer: DispatchSourceTimer?
    private let requestInterval: TimeInterval = 1.0

    private override init() {
        locationManager = CLLocationManager()
        super.init()
        locationManager.delegate = self
        locationManager.desiredAccuracy = kCLLocationAccuracyBest
        locationManager.distanceFilter = kCLDistanceFilterNone
        locationManager.allowsBackgroundLocationUpdates = true
        locationManager.pausesLocationUpdatesAutomatically = false
    }

    func start() {
        queue.async {
            if self.started {
                self.debugLog("start ignored (already started, enabled=\(self.deviceEnabled))")
                return
            }
            self.started = true
            self.debugLog("start triggered (enabled=\(self.deviceEnabled))")
            self.syncDeviceStateLocked()
        }
    }

    func setDeviceEnabled(_ enabled: Bool) {
        queue.async {
            self.deviceEnabled = enabled
            self.debugLog("setDeviceEnabled(\(enabled)) started=\(self.started)")
            self.syncDeviceStateLocked()
        }
    }

    private func syncDeviceStateLocked() {
        debugLog("syncDeviceStateLocked started=\(started) enabled=\(deviceEnabled)")
        PscalRuntimeBootstrap.shared.withRuntimeContext {
            PSCALRuntimeSetLocationDeviceEnabled(deviceEnabled ? 1 : 0)
        }
        if started && deviceEnabled {
            startLocationUpdatesLocked()
            startRequestTimerLocked()
        } else {
            stopLocationUpdatesLocked()
            stopRequestTimerLocked()
        }
    }

    private func startLocationUpdatesLocked() {
        guard !locationActive else { return }
        locationActive = true
        debugLog("starting location updates")
        DispatchQueue.main.async {
            self.requestAuthorizationIfNeeded()
            self.locationManager.startUpdatingLocation()
            self.requestLocationIfStaleLocked(force: true)
            if let initial = self.locationManager.location {
                self.queue.async { self.latestLocation = initial }
                self.queue.async { self.sendLatestLocationLocked() }
            }
        }
    }

    private func stopLocationUpdatesLocked() {
        guard locationActive else { return }
        locationActive = false
        debugLog("stopping location updates")
        DispatchQueue.main.async {
            self.locationManager.stopUpdatingLocation()
        }
    }

    private func startRequestTimerLocked() {
        guard requestTimer == nil else { return }
        let timer = DispatchSource.makeTimerSource(queue: queue)
        timer.schedule(deadline: .now() + requestInterval, repeating: requestInterval)
        timer.setEventHandler { [weak self] in
            self?.requestLocationIfStaleLocked(force: false)
        }
        timer.resume()
        requestTimer = timer
    }

    private func stopRequestTimerLocked() {
        requestTimer?.cancel()
        requestTimer = nil
    }

    private func sendLatestLocationLocked() {
        guard deviceEnabled, let location = latestLocation else { return }
        let payload = LocationDeviceProvider.createLocationPayload(location: location)
        sendPayload(payload)
    }

    private func sendPayload(_ payload: String) {
        guard let data = payload.data(using: .utf8), !data.isEmpty else { return }
        data.withUnsafeBytes { buffer in
            guard let base = buffer.baseAddress else { return }
            let res = PscalRuntimeBootstrap.shared.withRuntimeContext {
                PSCALRuntimeWriteLocationDevice(base.assumingMemoryBound(to: CChar.self),
                                                buffer.count)
            }
            if res < 0 {
                let err = errno
                if err != EAGAIN && err != EWOULDBLOCK && err != EPIPE {
                    runtimeDebugLog("Location device write failed: \(String(cString: strerror(err)))")
                }
                if debugEnabled {
                    debugLog("write failed len=\(buffer.count) errno=\(err)")
                }
            }
        }
    }

    private func requestAuthorizationIfNeeded() {
        let status = locationManager.authorizationStatus
        if status == .notDetermined {
            locationManager.requestAlwaysAuthorization()
        } else if status == .authorizedWhenInUse || status == .authorizedAlways {
            locationManager.startUpdatingLocation()
        }
    }

    func locationManager(_ manager: CLLocationManager, didChangeAuthorization status: CLAuthorizationStatus) {
        queue.async {
            if status == .denied || status == .restricted {
                self.debugLog("authorization restricted/denied; stopping updates")
                self.stopLocationUpdatesLocked()
            } else if status == .authorizedAlways || status == .authorizedWhenInUse {
                self.debugLog("authorization granted; starting updates")
                self.startLocationUpdatesLocked()
            }
        }
    }

    func locationManager(_ manager: CLLocationManager, didUpdateLocations locations: [CLLocation]) {
        guard let latest = locations.last else { return }
        queue.async {
            if self.debugEnabled {
                self.debugLog(String(format: "didUpdateLocations lat=%.5f lon=%.5f",
                                     latest.coordinate.latitude,
                                     latest.coordinate.longitude))
            }
            self.latestLocation = latest
            self.sendLatestLocationLocked()
        }
    }

    func locationManager(_ manager: CLLocationManager, didFailWithError error: Error) {
        runtimeDebugLog("Location device error: \(error.localizedDescription)")
        if debugEnabled {
            debugLog("didFailWithError \(error.localizedDescription)")
        }
    }

    private static func createLocationPayload(location: CLLocation) -> String {
        let lat = location.coordinate.latitude
        let lon = location.coordinate.longitude
        return String(format: "%+.6f,%+.6f\n", lat, lon)
    }

    private func debugLog(_ message: String) {
        guard debugEnabled else { return }
        runtimeDebugLog("[location] \(message)")
    }

    private func requestLocationIfStaleLocked(force: Bool) {
        let now = Date().timeIntervalSinceReferenceDate
        if !force && now - lastLocationRequest < locationRequestInterval {
            return
        }
        lastLocationRequest = now
        DispatchQueue.main.async {
            self.locationManager.requestLocation()
        }
    }
}
