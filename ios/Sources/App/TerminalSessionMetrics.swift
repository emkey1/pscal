import Foundation
import Darwin

struct TerminalSessionMetricsSnapshot {
    let visible: Bool
    let htermAttached: Bool
    let sessionOutputPaused: Bool
    let attachedOutputBytes: Int
    let pauseTransitions: Int
    let terminalBuffer: TerminalBuffer.BufferMetrics
    let htermOutput: HtermTerminalController.OutputMetrics
}

final class TerminalSessionMetricsLogger {
    private struct Fingerprint: Equatable {
        let visible: Bool
        let htermAttached: Bool
        let sessionOutputPaused: Bool
        let terminalBucket: Int
        let scrollbackBucket: Int
        let bufferedInputBucket: Int
        let attachedBucket: Int
        let htermPendingBucket: Int
        let trimCount: Int
        let reloadCount: Int
        let discardCount: Int
        let pauseTransitions: Int
        let usingAlternateScreen: Bool

        init(snapshot: TerminalSessionMetricsSnapshot) {
            visible = snapshot.visible
            htermAttached = snapshot.htermAttached
            sessionOutputPaused = snapshot.sessionOutputPaused
            terminalBucket = snapshot.terminalBuffer.estimatedStorageBytes / (64 * 1024)
            scrollbackBucket = snapshot.terminalBuffer.scrollbackRows / 50
            bufferedInputBucket = snapshot.terminalBuffer.bufferedInputBytes / (32 * 1024)
            attachedBucket = snapshot.attachedOutputBytes / (64 * 1024)
            htermPendingBucket = snapshot.htermOutput.pendingBytes / (64 * 1024)
            trimCount = snapshot.htermOutput.trimCount
            reloadCount = snapshot.htermOutput.reloadCount
            discardCount = snapshot.htermOutput.discardCount
            pauseTransitions = snapshot.pauseTransitions
            usingAlternateScreen = snapshot.terminalBuffer.usingAlternateScreen
        }
    }

    private let sessionKind: String
    private let sessionId: UInt64
    private let minimumLogInterval: TimeInterval = 5.0
    private let periodicLogInterval: TimeInterval = 15.0
    private var lastLogTime: TimeInterval = 0
    private var lastFingerprint: Fingerprint?
    private var peakTerminalBytes: Int = 0
    private var peakAttachedBytes: Int = 0
    private var peakHtermPendingBytes: Int = 0

    init(sessionKind: String, sessionId: UInt64) {
        self.sessionKind = sessionKind
        self.sessionId = sessionId
    }

    func record(_ snapshot: TerminalSessionMetricsSnapshot, reason: String, force: Bool = false) {
        peakTerminalBytes = max(peakTerminalBytes, snapshot.terminalBuffer.estimatedStorageBytes)
        peakAttachedBytes = max(peakAttachedBytes, snapshot.attachedOutputBytes)
        peakHtermPendingBytes = max(peakHtermPendingBytes, snapshot.htermOutput.pendingBytes)

        let fingerprint = Fingerprint(snapshot: snapshot)
        let now = Date().timeIntervalSince1970
        let hasBufferedState = snapshot.attachedOutputBytes > 0 ||
            snapshot.htermOutput.pendingBytes > 0 ||
            snapshot.terminalBuffer.bufferedInputBytes > 0
        let significantChange = fingerprint != lastFingerprint
        let timeSinceLastLog = now - lastLogTime
        let periodicSampleDue = timeSinceLastLog >= periodicLogInterval
        let intervalSampleDue = timeSinceLastLog >= minimumLogInterval && (significantChange || hasBufferedState)
        guard force || periodicSampleDue || intervalSampleDue else {
            return
        }
        lastLogTime = now
        lastFingerprint = fingerprint

        let rssBytes = processResidentMemoryBytes() ?? 0
        let message = "[TermMetrics] kind=\(sessionKind) session=\(sessionId) reason=\(reason) visible=\(snapshot.visible ? 1 : 0) attached=\(snapshot.htermAttached ? 1 : 0) paused=\(snapshot.sessionOutputPaused ? 1 : 0) pause_transitions=\(snapshot.pauseTransitions) terminal_bytes=\(snapshot.terminalBuffer.estimatedStorageBytes) scrollback_rows=\(snapshot.terminalBuffer.scrollbackRows) buffered_input_bytes=\(snapshot.terminalBuffer.bufferedInputBytes) columns=\(snapshot.terminalBuffer.columns) rows=\(snapshot.terminalBuffer.rows) alt_screen=\(snapshot.terminalBuffer.usingAlternateScreen ? 1 : 0) attached_backlog_bytes=\(snapshot.attachedOutputBytes) hterm_pending_bytes=\(snapshot.htermOutput.pendingBytes) hterm_loaded=\(snapshot.htermOutput.isLoaded ? 1 : 0) hterm_host_ready=\(snapshot.htermOutput.hostSizeReady ? 1 : 0) hterm_trims=\(snapshot.htermOutput.trimCount) hterm_reloads=\(snapshot.htermOutput.reloadCount) hterm_discards=\(snapshot.htermOutput.discardCount) peak_terminal_bytes=\(peakTerminalBytes) peak_attached_backlog_bytes=\(peakAttachedBytes) peak_hterm_pending_bytes=\(peakHtermPendingBytes) rss_bytes=\(rssBytes)"
        runtimeDebugLog(message)
        NSLog("%@", message)
    }

    private func processResidentMemoryBytes() -> UInt64? {
        var info = mach_task_basic_info()
        var count = mach_msg_type_number_t(MemoryLayout<mach_task_basic_info>.size / MemoryLayout<integer_t>.size)
        let kernResult: kern_return_t = withUnsafeMutablePointer(to: &info) { pointer in
            pointer.withMemoryRebound(to: integer_t.self, capacity: Int(count)) { reboundPointer in
                task_info(mach_task_self_,
                          task_flavor_t(MACH_TASK_BASIC_INFO),
                          reboundPointer,
                          &count)
            }
        }
        guard kernResult == KERN_SUCCESS else {
            return nil
        }
        return UInt64(info.resident_size)
    }
}
