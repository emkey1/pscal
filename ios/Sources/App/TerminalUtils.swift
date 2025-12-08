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
