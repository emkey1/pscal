import Combine
import Foundation
import Darwin

final class PscalRuntimeBootstrap: ObservableObject {
    static let shared = PscalRuntimeBootstrap()

    @Published private(set) var screenText: NSAttributedString = NSAttributedString(string: "Launching exsh...")
    @Published private(set) var exitStatus: Int32?

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
        self.terminalBuffer = TerminalBuffer(columns: initialColumns, rows: initialRows, scrollback: 400)
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

    private func scheduleRender() {
        let snapshot = terminalBuffer.snapshot()
        DispatchQueue.main.async {
            self.screenText = PscalRuntimeBootstrap.renderJoined(snapshot: snapshot)
        }
    }

    private static func renderJoined(snapshot: TerminalBuffer.TerminalSnapshot) -> NSAttributedString {
        let lines = TerminalBuffer.render(snapshot: snapshot)
        let result = NSMutableAttributedString()
        for (index, line) in lines.enumerated() {
            result.append(line)
            if index < lines.count - 1 {
                result.append(NSAttributedString(string: "\n"))
            }
        }
        return result
    }
}
