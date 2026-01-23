import Foundation

/// Safely passes a Swift string to a C function that expects a const char *.
func withCStringPointer(_ string: String, _ body: (UnsafePointer<CChar>) -> Void) {
    string.withCString { ptr in
        body(ptr)
    }
}

/// Convenience wrapper for emitting terminal-related debug logs.
func terminalViewLog(_ message: String) {
    message.withCString { ptr in
        pscalRuntimeDebugLog(ptr)
    }
}

enum TerminalDebugFlags {
    static let printChanges: Bool = {
        let env = ProcessInfo.processInfo.environment
        if let raw = env["PSCALI_PRINT_CHANGES"], raw != "0" {
            return true
        }
        if ProcessInfo.processInfo.arguments.contains("--pscali-print-changes") {
            return true
        }
        if UserDefaults.standard.bool(forKey: "PSCALI_PRINT_CHANGES") {
            return true
        }
        return false
    }()
}

func traceViewChanges(_ name: String) {
    guard TerminalDebugFlags.printChanges else { return }
    let message = "[ViewChanges] \(name)"
    print(message)
    terminalViewLog(message)
}

private let sshDebugEnabled: Bool = {
    let env = ProcessInfo.processInfo.environment
    if let value = env["PSCALI_SSH_DEBUG"], !value.isEmpty, value != "0" {
        return true
    }
    if let value = env["PSCALI_TOOL_DEBUG"], !value.isEmpty, value != "0" {
        return true
    }
    return false
}()

func sshDebugLog(_ message: String) {
    guard sshDebugEnabled else { return }
    terminalViewLog(message)
}
