import Foundation

@_silgen_name("pscalRuntimeDebugLog")
func c_terminalDebugLog(_ message: UnsafePointer<CChar>) -> Void

func withCStringPointer(_ string: String, _ body: (UnsafePointer<CChar>) -> Void) {
    let utf8 = string.utf8CString
    utf8.withUnsafeBufferPointer { buffer in
        if let base = buffer.baseAddress {
            body(base)
        }
    }
}

func terminalViewLog(_ message: String) {
    withCStringPointer(message) { c_terminalDebugLog($0) }
}
