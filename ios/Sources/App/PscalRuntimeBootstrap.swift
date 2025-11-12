import Combine
import Foundation

final class PscalRuntimeBootstrap: ObservableObject {
    static let shared = PscalRuntimeBootstrap()

    @Published private(set) var screenLines: [NSAttributedString] = [NSAttributedString(string: "Launching exsh...")]
    @Published private(set) var exitStatus: Int32?

    private var started = false
    private let stateQueue = DispatchQueue(label: "com.pscal.runtime.state")
    private let launchQueue = DispatchQueue(label: "com.pscal.runtime.launch", qos: .userInitiated)
    private var handlerContext: UnsafeMutableRawPointer?
    private let terminalBuffer = TerminalBuffer(columns: 100, rows: 32, scrollback: 400)

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

    func start() {
        let shouldStart = stateQueue.sync { () -> Bool in
            guard !started else { return false }
            started = true
            return true
        }
        guard shouldStart else { return }

        RuntimeAssetInstaller.shared.prepareWorkspace()

        DispatchQueue.main.async {
            self.screenLines = [NSAttributedString(string: "Launching exsh...")]
            self.exitStatus = nil
        }

        handlerContext = Unmanaged.passUnretained(self).toOpaque()
        let documentsPath = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask).first?.appendingPathComponent("asan.log").path
        let logPath = documentsPath ?? "/tmp/asan.log"
        let asanOptions = "abort_on_error=1:halt_on_error=1:log_path=\(logPath)"
        _ = asanOptions.withCString { cString in
            setenv("ASAN_OPTIONS", cString, 1)
        }
        _ = logPath.withCString { cString in
            PSCALRuntimeConfigureAsanReportPath(cString)
        }
        setenv("ASAN_SYMBOLIZER_PATH", "/usr/bin/atos", 1)
        PSCALRuntimeConfigureHandlers(outputHandler, exitHandler, handlerContext)

        launchQueue.async {
            let args = ["exsh"]
            var cArgs: [UnsafeMutablePointer<CChar>?] = args.map { strdup($0) }
            defer {
                cArgs.forEach { if let ptr = $0 { free(ptr) } }
            }
            cArgs.withUnsafeMutableBufferPointer { buffer in
                _ = PSCALRuntimeLaunchExsh(Int32(buffer.count), buffer.baseAddress)
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

    @objc
    public func consumeOutput(buffer: UnsafePointer<Int8>, length: Int) {
        
        // 1. THE CRITICAL FIX: Create a safe, deep copy of the C-buffer's data.
        let dataCopy = Data(bytes: buffer, count: length)

        // 2. Dispatch all UI-related work to the main thread.
        DispatchQueue.main.async {
            
            // 3. (FIXED) Append the safe data to the terminal buffer.
            self.terminalBuffer.append(data: dataCopy)
            
            // 4. Get a snapshot of the buffer's new state.
            let snapshot = self.terminalBuffer.snapshot()
            
            // 5. Render that snapshot into strings and update the UI.
            self.screenLines = TerminalBuffer.render(snapshot: snapshot)
        }
    }

    private func handleExit(status: Int32) {
        stateQueue.async {
            self.started = false
        }
        DispatchQueue.main.async {
            self.exitStatus = status
        }
    }
}
