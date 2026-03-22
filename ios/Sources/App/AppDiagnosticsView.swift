import SwiftUI
import UIKit
import Foundation

private enum AppDiagnosticsStatus: String {
    case pass = "PASS"
    case warn = "WARN"
    case fail = "FAIL"

    var systemImageName: String {
        switch self {
        case .pass:
            return "checkmark.circle.fill"
        case .warn:
            return "exclamationmark.triangle.fill"
        case .fail:
            return "xmark.octagon.fill"
        }
    }

    var color: Color {
        switch self {
        case .pass:
            return .green
        case .warn:
            return .orange
        case .fail:
            return .red
        }
    }
}

private struct AppDiagnosticsCheckResult: Identifiable {
    let id = UUID()
    let name: String
    let status: AppDiagnosticsStatus
    let summary: String
    let details: String
}

private struct AppDiagnosticsRunResult {
    let startedAt: Date
    let finishedAt: Date
    let checks: [AppDiagnosticsCheckResult]
    let report: String
}

private struct AppDiagnosticsShellProbe {
    let sessionId: UInt64
    let argv: [String]
    let transcript: String
    let exitStatus: Int32?
    let errnoValue: Int32
    let usingAlternateScreenAtEnd: Bool
    let finalColumns: Int
    let finalRows: Int
}

@MainActor
private final class AppDiagnosticsRunner: ObservableObject {
    @Published private(set) var isRunning: Bool = false
    @Published private(set) var checks: [AppDiagnosticsCheckResult] = []
    @Published private(set) var report: String = ""
    @Published private(set) var lastRunDescription: String = "Not run yet"

    func run() {
        guard !isRunning else { return }
        isRunning = true
        checks = []
        report = ""
        lastRunDescription = "Running..."

        Task {
            let result = await Self.performRun()
            await MainActor.run {
                self.isRunning = false
                self.checks = result.checks
                self.report = result.report
                let formatter = DateFormatter()
                formatter.dateStyle = .medium
                formatter.timeStyle = .medium
                self.lastRunDescription = formatter.string(from: result.finishedAt)
            }
        }
    }

    func copyReportToPasteboard() {
        guard !report.isEmpty else { return }
        UIPasteboard.general.string = report
    }

    private static func performRun() async -> AppDiagnosticsRunResult {
        let startedAt = Date()
        var checks: [AppDiagnosticsCheckResult] = []

        RuntimeAssetInstaller.shared.prepareWorkspace()

        checks.append(bundleResourceCheck())
        checks.append(workspaceLayoutCheck())
        checks.append(toolRunnerCheck())
        checks.append(writablePathsCheck())
        checks.append(fontCatalogCheck())
        checks.append(terminalWebCheck())
        checks.append(await shellSmokeCheck())
        checks.append(await concurrentShellStartupCheck())
        checks.append(await resizeStressCheck())
        checks.append(await standaloneExitRoutingCheck())
        checks.append(await shellFileRoundTripCheck())
        checks.append(await pathVirtualizationCheck())
        checks.append(await toolBuiltinFallbackCheck())
        checks.append(await terminalCapabilityCheck())
        checks.append(await multiSessionIsolationCheck())
        checks.append(await frontendExecutionSmokeCheck())

        let finishedAt = Date()
        let report = buildReport(startedAt: startedAt, finishedAt: finishedAt, checks: checks)
        return AppDiagnosticsRunResult(startedAt: startedAt,
                                       finishedAt: finishedAt,
                                       checks: checks,
                                       report: report)
    }

    private static func buildReport(startedAt: Date,
                                    finishedAt: Date,
                                    checks: [AppDiagnosticsCheckResult]) -> String {
        let bundle = Bundle.main
        let info = ProcessInfo.processInfo
        let device = UIDevice.current
        let marketingVersion = bundle.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String ?? "?"
        let buildVersion = bundle.object(forInfoDictionaryKey: "CFBundleVersion") as? String ?? "?"
        let formatter = ISO8601DateFormatter()
        formatter.formatOptions = [.withInternetDateTime, .withFractionalSeconds]
        let passCount = checks.filter { $0.status == .pass }.count
        let warnCount = checks.filter { $0.status == .warn }.count
        let failCount = checks.filter { $0.status == .fail }.count
        let overall = failCount == 0 ? (warnCount == 0 ? "PASS" : "WARN") : "FAIL"

        var lines: [String] = []
        lines.append("PSCAL iOS/iPadOS Diagnostics")
        lines.append("Overall: \(overall)")
        lines.append("App version: \(marketingVersion) (\(buildVersion))")
        lines.append("Platform: \(device.systemName) \(device.systemVersion)")
        lines.append("Device: \(device.model) idiom=\(idiomName(device.userInterfaceIdiom))")
        lines.append("OS build: \(info.operatingSystemVersionString)")
        lines.append("Started: \(formatter.string(from: startedAt))")
        lines.append("Finished: \(formatter.string(from: finishedAt))")
        lines.append("Summary: pass=\(passCount) warn=\(warnCount) fail=\(failCount)")
        lines.append("")

        for check in checks {
            lines.append("[\(check.status.rawValue)] \(check.name): \(check.summary)")
            if !check.details.isEmpty {
                for line in check.details.split(separator: "\n", omittingEmptySubsequences: false) {
                    lines.append("  \(line)")
                }
            }
            lines.append("")
        }

        let runtimeLog = runtimeLogExcerpt()
        if !runtimeLog.isEmpty {
            lines.append("Recent Runtime Log")
            lines.append("------------------")
            lines.append(runtimeLog)
            lines.append("")
        }

        return lines.joined(separator: "\n")
    }

    private static func idiomName(_ idiom: UIUserInterfaceIdiom) -> String {
        switch idiom {
        case .phone:
            return "phone"
        case .pad:
            return "pad"
        case .mac:
            return "mac"
        case .tv:
            return "tv"
        case .vision:
            return "vision"
        default:
            return "unspecified"
        }
    }

    private static func documentsDirectory() -> URL {
        FileManager.default.urls(for: .documentDirectory, in: .userDomainMask).first
            ?? URL(fileURLWithPath: NSTemporaryDirectory(), isDirectory: true)
    }

    private static func existenceDescription(_ url: URL, isDirectory: Bool) -> String {
        let fileManager = FileManager.default
        var directoryFlag = ObjCBool(false)
        let exists = fileManager.fileExists(atPath: url.path, isDirectory: &directoryFlag)
        if !exists {
            return "missing: \(url.path)"
        }
        if directoryFlag.boolValue != isDirectory {
            return "wrong type: \(url.path)"
        }
        return "ok: \(url.path)"
    }

    private static func bundleResourceCheck() -> AppDiagnosticsCheckResult {
        guard let bundleRoot = Bundle.main.resourceURL else {
            return AppDiagnosticsCheckResult(name: "Bundle resources",
                                             status: .fail,
                                             summary: "missing app bundle resource root",
                                             details: "")
        }

        let targets: [(String, URL, Bool)] = [
            ("Examples", bundleRoot.appendingPathComponent("Examples", isDirectory: true), true),
            ("Docs", bundleRoot.appendingPathComponent("Docs", isDirectory: true), true),
            ("TerminalWeb/term.html", bundleRoot.appendingPathComponent("TerminalWeb/term.html", isDirectory: false), false),
            ("TerminalWeb/hterm_all.js", bundleRoot.appendingPathComponent("TerminalWeb/hterm_all.js", isDirectory: false), false),
            ("fonts", bundleRoot.appendingPathComponent("fonts", isDirectory: true), true),
            ("pscal_tool_runner.deflate", bundleRoot.appendingPathComponent("pscal_tool_runner.deflate", isDirectory: false), false),
        ]

        let failures = targets.filter { !FileManager.default.fileExists(atPath: $0.1.path) }
        let details = targets.map { target in
            "\(target.0): \(existenceDescription(target.1, isDirectory: target.2))"
        }.joined(separator: "\n")

        if failures.isEmpty {
            return AppDiagnosticsCheckResult(name: "Bundle resources",
                                             status: .pass,
                                             summary: "required bundled assets are present",
                                             details: details)
        }

        return AppDiagnosticsCheckResult(name: "Bundle resources",
                                         status: .fail,
                                         summary: "missing \(failures.count) required bundled asset(s)",
                                         details: details)
    }

    private static func workspaceLayoutCheck() -> AppDiagnosticsCheckResult {
        let docs = documentsDirectory()
        let targets: [(String, URL, Bool)] = [
            ("Documents/home", docs.appendingPathComponent("home", isDirectory: true), true),
            ("Documents/home/Examples", docs.appendingPathComponent("home/Examples", isDirectory: true), true),
            ("Documents/home/Docs", docs.appendingPathComponent("home/Docs", isDirectory: true), true),
            ("Documents/bin", docs.appendingPathComponent("bin", isDirectory: true), true),
            ("Documents/fonts", docs.appendingPathComponent("fonts", isDirectory: true), true),
            ("Documents/var/log", docs.appendingPathComponent("var/log", isDirectory: true), true),
        ]

        let missing = targets.filter { !FileManager.default.fileExists(atPath: $0.1.path) }
        let details = targets.map { target in
            "\(target.0): \(existenceDescription(target.1, isDirectory: target.2))"
        }.joined(separator: "\n")

        if missing.isEmpty {
            return AppDiagnosticsCheckResult(name: "Workspace staging",
                                             status: .pass,
                                             summary: "runtime workspace directories are installed",
                                             details: details)
        }

        return AppDiagnosticsCheckResult(name: "Workspace staging",
                                         status: .fail,
                                         summary: "missing \(missing.count) staged workspace path(s)",
                                         details: details)
    }

    private static func toolRunnerCheck() -> AppDiagnosticsCheckResult {
        let bundleHasDeflated = Bundle.main.url(forResource: "pscal_tool_runner",
                                                withExtension: "deflate") != nil
#if os(iOS)
        let status: AppDiagnosticsStatus = bundleHasDeflated ? .pass : .fail
        let summary = bundleHasDeflated
            ? "tool runner payload is bundled; iOS uses inline fallback instead of fork/exec"
            : "pscal_tool_runner.deflate is missing from the app bundle"
        let details = [
            "bundle payload present: \(bundleHasDeflated)",
            "fork/exec path required: false",
            "inline fallback path: active for iOS shell tool dispatch"
        ].joined(separator: "\n")
        return AppDiagnosticsCheckResult(name: "Tool runner staging",
                                         status: status,
                                         summary: summary,
                                         details: details)
#else
        guard let path = RuntimeAssetInstaller.shared.ensureToolRunnerExecutable() else {
            return AppDiagnosticsCheckResult(name: "Tool runner staging",
                                             status: .fail,
                                             summary: "pscal_tool_runner could not be staged",
                                             details: "RuntimeAssetInstaller.shared.ensureToolRunnerExecutable() returned nil")
        }
        let fileManager = FileManager.default
        let executable = fileManager.isExecutableFile(atPath: path)
        return AppDiagnosticsCheckResult(name: "Tool runner staging",
                                         status: executable ? .pass : .fail,
                                         summary: executable ? "tool runner staged and executable" : "staged tool runner is not executable",
                                         details: path)
#endif
    }

    private static func writablePathsCheck() -> AppDiagnosticsCheckResult {
        let fileManager = FileManager.default
        let docs = documentsDirectory()
        let targets: [(String, URL)] = [
            ("Documents/tmp", docs.appendingPathComponent("tmp", isDirectory: true)),
            ("Documents/var/log", docs.appendingPathComponent("var/log", isDirectory: true)),
            ("Documents/home", docs.appendingPathComponent("home", isDirectory: true)),
        ]

        var failures: [String] = []
        var lines: [String] = []
        for (name, directory) in targets {
            do {
                try fileManager.createDirectory(at: directory, withIntermediateDirectories: true)
                let probe = directory.appendingPathComponent(".pscal_diag_probe_\(UUID().uuidString)", isDirectory: false)
                if let data = "probe".data(using: .utf8) {
                    try data.write(to: probe, options: [.atomic])
                }
                _ = try String(contentsOf: probe, encoding: .utf8)
                try fileManager.removeItem(at: probe)
                lines.append("\(name): writable")
            } catch {
                failures.append(name)
                lines.append("\(name): \(error.localizedDescription)")
            }
        }

        return AppDiagnosticsCheckResult(name: "Writable sandbox paths",
                                         status: failures.isEmpty ? .pass : .fail,
                                         summary: failures.isEmpty ? "tmp, home, and log directories are writable" : "failed writable probes: \(failures.joined(separator: ", "))",
                                         details: lines.joined(separator: "\n"))
    }

    private static func fontCatalogCheck() -> AppDiagnosticsCheckResult {
        let settings = TerminalFontSettings.shared
        let count = settings.fontOptions.count
        let currentFont = settings.currentFont
        let details = [
            "font options: \(count)",
            "selected font id: \(settings.selectedFontID)",
            "resolved font: \(currentFont.fontName)",
            "point size: \(Int(settings.pointSize))",
        ].joined(separator: "\n")

        if count > 0 {
            return AppDiagnosticsCheckResult(name: "Font catalog",
                                             status: .pass,
                                             summary: "terminal font settings resolved correctly",
                                             details: details)
        }

        return AppDiagnosticsCheckResult(name: "Font catalog",
                                         status: .fail,
                                         summary: "no terminal fonts were available",
                                         details: details)
    }

    private static func terminalWebCheck() -> AppDiagnosticsCheckResult {
        let targets = [
            Bundle.main.url(forResource: "term.html", withExtension: nil, subdirectory: "TerminalWeb"),
            Bundle.main.url(forResource: "hterm_all", withExtension: "js", subdirectory: "TerminalWeb"),
        ]
        let ok = targets.allSatisfy { $0 != nil }
        let details = [
            "term.html: \(targets[0]?.path ?? "missing")",
            "hterm_all.js: \(targets[1]?.path ?? "missing")",
        ].joined(separator: "\n")

        return AppDiagnosticsCheckResult(name: "Terminal web assets",
                                         status: ok ? .pass : .fail,
                                         summary: ok ? "terminal HTML/JS resources are loadable" : "terminal HTML/JS resources are missing",
                                         details: details)
    }

    private static func shellQuote(_ text: String) -> String {
        "'" + text.replacingOccurrences(of: "'", with: "'\"'\"'") + "'"
    }

    private static func runtimeLogExcerpt(maxLines: Int = 80, maxChars: Int = 8000) -> String {
        guard let ptr = pscalRuntimeCopySessionLog() else {
            return ""
        }
        defer { free(ptr) }
        let raw = String(cString: ptr)
        let lines = raw.split(separator: "\n", omittingEmptySubsequences: false)
        let suffix = lines.suffix(maxLines)
        var excerpt = suffix.joined(separator: "\n")
        excerpt = excerpt.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !excerpt.isEmpty else {
            return ""
        }
        if excerpt.count > maxChars {
            let startIndex = excerpt.index(excerpt.endIndex, offsetBy: -maxChars)
            excerpt = "...[truncated]...\n" + excerpt[startIndex...]
        }
        return excerpt
    }

    private static func runShellProbe(argv: [String],
                                      timeout: TimeInterval = 8.0,
                                      afterStart: (@MainActor (ShellRuntimeSession) async -> Void)? = nil) async -> AppDiagnosticsShellProbe {
        let sessionId = PSCALRuntimeNextSessionId()
        let session = ShellRuntimeSession(sessionId: sessionId,
                                          argv: argv,
                                          program: .shell)
        session.setViewVisible(true)
        session.updateTerminalSize(columns: 80, rows: 24)

        let started = session.start()
        guard started else {
            let errnoValue = session.lastStartErrno
            return AppDiagnosticsShellProbe(sessionId: sessionId,
                                            argv: argv,
                                            transcript: "",
                                            exitStatus: nil,
                                            errnoValue: errnoValue,
                                            usingAlternateScreenAtEnd: false,
                                            finalColumns: 80,
                                            finalRows: 24)
        }

        if let afterStart {
            Task { @MainActor in
                await afterStart(session)
            }
        }

        let deadline = Date().addingTimeInterval(timeout)
        var transcript = ""
        var exitStatus = session.exitStatus
        while Date() < deadline {
            transcript = session.screenText.string
            exitStatus = session.exitStatus
            if exitStatus != nil {
                break
            }
            try? await Task.sleep(nanoseconds: 150_000_000)
        }

        if exitStatus == nil {
            session.requestClose()
            try? await Task.sleep(nanoseconds: 400_000_000)
            exitStatus = session.exitStatus
            transcript = session.screenText.string
        } else {
            // Give the PTY/output pipeline a brief chance to flush trailing bytes
            // after process exit so the probe sees the final line reliably.
            try? await Task.sleep(nanoseconds: 150_000_000)
            exitStatus = session.exitStatus
            transcript = session.screenText.string
        }

        let metrics = session.terminalBufferMetrics()
        return AppDiagnosticsShellProbe(sessionId: sessionId,
                                        argv: argv,
                                        transcript: sanitizeTranscript(transcript),
                                        exitStatus: exitStatus,
                                        errnoValue: 0,
                                        usingAlternateScreenAtEnd: metrics.usingAlternateScreen,
                                        finalColumns: metrics.columns,
                                        finalRows: metrics.rows)
    }

    private static func shellSmokeCheck() async -> AppDiagnosticsCheckResult {
        let command = "echo __PSCAL_IOS_DIAG_BEGIN__; pwd; echo __PSCAL_IOS_DIAG_END__"
        let probe = await runShellProbe(argv: ["exsh", "-c", command])
        if probe.errnoValue != 0 {
            return AppDiagnosticsCheckResult(name: "Shell session smoke test",
                                             status: .fail,
                                             summary: "failed to start isolated shell session",
                                             details: "errno=\(probe.errnoValue)")
        }

        let sanitizedTranscript = probe.transcript
        let ok = sanitizedTranscript.contains("__PSCAL_IOS_DIAG_BEGIN__") &&
            sanitizedTranscript.contains("__PSCAL_IOS_DIAG_END__")
        let status: AppDiagnosticsStatus
        let summary: String
        if ok && probe.exitStatus == 0 {
            status = .pass
            summary = "shell launched, accepted input, and exited cleanly"
        } else if ok {
            status = .warn
            summary = "shell launched and echoed markers, but exit status was \(probe.exitStatus.map(String.init) ?? "nil")"
        } else {
            status = .fail
            summary = "shell transcript did not include expected diagnostic markers"
        }

        let details = [
            "session id: \(probe.sessionId)",
            "argv: \(probe.argv.joined(separator: " "))",
            "exit status: \(probe.exitStatus.map(String.init) ?? "nil")",
            "transcript:",
            sanitizedTranscript.isEmpty ? "(empty)" : sanitizedTranscript,
        ].joined(separator: "\n")

        return AppDiagnosticsCheckResult(name: "Shell session smoke test",
                                         status: status,
                                         summary: summary,
                                         details: details)
    }

    private static func concurrentShellStartupCheck() async -> AppDiagnosticsCheckResult {
        let commands = [
            "echo __DIAG_CONCURRENT_1__; pwd; echo __DIAG_DONE_1__",
            "echo __DIAG_CONCURRENT_2__; pwd; echo __DIAG_DONE_2__",
            "echo __DIAG_CONCURRENT_3__; pwd; echo __DIAG_DONE_3__"
        ]
        async let probe1 = runShellProbe(argv: ["exsh", "-c", commands[0]])
        async let probe2 = runShellProbe(argv: ["exsh", "-c", commands[1]])
        async let probe3 = runShellProbe(argv: ["exsh", "-c", commands[2]])
        let probes = await [probe1, probe2, probe3]

        let failures = probes.enumerated().filter { index, probe in
            let startMarker = "__DIAG_CONCURRENT_\(index + 1)__"
            return probe.errnoValue != 0 || probe.exitStatus != 0 || !probe.transcript.contains(startMarker)
        }
        let details = probes.enumerated().map { index, probe in
            let startMarker = "__DIAG_CONCURRENT_\(index + 1)__"
            let ok = probe.errnoValue == 0 && probe.exitStatus == 0 && probe.transcript.contains(startMarker)
            return [
                "session \(index + 1): \(ok ? "ok" : "failed")",
                "  session id: \(probe.sessionId)",
                "  exit status: \(probe.exitStatus.map(String.init) ?? "nil")",
                "  errno: \(probe.errnoValue)",
                "  transcript: \(probe.transcript.isEmpty ? "(empty)" : probe.transcript)"
            ].joined(separator: "\n")
        }.joined(separator: "\n")

        return AppDiagnosticsCheckResult(name: "Concurrent shell startup",
                                         status: failures.isEmpty ? .pass : .fail,
                                         summary: failures.isEmpty
                                             ? "multiple isolated shell sessions launched and exited cleanly"
                                             : "\(failures.count) concurrent shell session(s) failed",
                                         details: details)
    }

    private static func resizeStressCheck() async -> AppDiagnosticsCheckResult {
        let command = "echo __DIAG_RESIZE_BEGIN__; echo line1; echo line2; echo line3; echo __DIAG_RESIZE_END__"
        let probe = await runShellProbe(argv: ["exsh", "-c", command], afterStart: { session in
            let sizes = [(90, 30), (72, 20), (100, 28), (80, 24)]
            for (cols, rows) in sizes {
                session.updateTerminalSize(columns: cols, rows: rows)
                try? await Task.sleep(nanoseconds: 50_000_000)
            }
        })

        let ok = probe.errnoValue == 0 &&
            probe.exitStatus == 0 &&
            probe.transcript.contains("__DIAG_RESIZE_BEGIN__") &&
            probe.transcript.contains("__DIAG_RESIZE_END__")
        let details = [
            "session id: \(probe.sessionId)",
            "exit status: \(probe.exitStatus.map(String.init) ?? "nil")",
            "errno: \(probe.errnoValue)",
            "transcript:",
            probe.transcript.isEmpty ? "(empty)" : probe.transcript
        ].joined(separator: "\n")
        return AppDiagnosticsCheckResult(name: "Terminal resize stress",
                                         status: ok ? .pass : .fail,
                                         summary: ok
                                             ? "shell stayed responsive through repeated terminal resizes"
                                             : "shell did not complete cleanly during resize stress",
                                         details: details)
    }

    private static func standaloneExitRoutingCheck() async -> AppDiagnosticsCheckResult {
        let probe = await runShellProbe(argv: ["exsh", "-c", "exit 7"])
        let ok = probe.errnoValue == 0 && probe.exitStatus == 7
        let details = [
            "session id: \(probe.sessionId)",
            "exit status: \(probe.exitStatus.map(String.init) ?? "nil")",
            "errno: \(probe.errnoValue)"
        ].joined(separator: "\n")
        return AppDiagnosticsCheckResult(name: "Standalone session exit routing",
                                         status: ok ? .pass : .fail,
                                         summary: ok
                                             ? "detached shell session propagated its exit status correctly"
                                             : "detached shell session exit did not propagate as expected",
                                         details: details)
    }

    private static func shellFileRoundTripCheck() async -> AppDiagnosticsCheckResult {
        let probeFile = documentsDirectory()
            .appendingPathComponent("tmp", isDirectory: true)
            .appendingPathComponent("diag_roundtrip.txt", isDirectory: false)
        let quotedPath = shellQuote(probeFile.path)
        let command = "echo __ROUNDTRIP__ > \(quotedPath); cat \(quotedPath)"
        let probe = await runShellProbe(argv: ["exsh", "-c", command])
        let fileContents = (try? String(contentsOf: probeFile, encoding: .utf8))?
            .trimmingCharacters(in: .whitespacesAndNewlines)
        try? FileManager.default.removeItem(at: probeFile)
        let shellRoundTripOK = probe.errnoValue == 0 &&
            probe.exitStatus == 0 &&
            probe.transcript.contains("__ROUNDTRIP__")
        let hostVisible = fileContents == "__ROUNDTRIP__"
        let details = [
            "session id: \(probe.sessionId)",
            "path: \(probeFile.path)",
            "exit status: \(probe.exitStatus.map(String.init) ?? "nil")",
            "errno: \(probe.errnoValue)",
            "host-visible file contents: \(fileContents ?? "(nil)")",
            "host-visible match: \(hostVisible)",
            "transcript:",
            probe.transcript.isEmpty ? "(empty)" : probe.transcript
        ].joined(separator: "\n")
        return AppDiagnosticsCheckResult(name: "Shell file round-trip",
                                         status: shellRoundTripOK ? .pass : .fail,
                                         summary: shellRoundTripOK
                                             ? "shell wrote and read a file in the sandboxed workspace"
                                             : "shell file round-trip did not complete as expected",
                                         details: details)
    }

    private static func pathVirtualizationCheck() async -> AppDiagnosticsCheckResult {
        let command = """
        printf '__PATHVIRT_BEGIN__\\n'; \
        printf 'HOME=%s\\n' "$HOME"; \
        printf 'PWD=%s\\n' "$PWD"; \
        printf 'TMPDIR=%s\\n' "$TMPDIR"; \
        printf 'PSCALI_WORKDIR=%s\\n' "$PSCALI_WORKDIR"; \
        cd /home/Docs && printf 'DOCS_PWD=%s\\n' "$PWD"; \
        printf '__VIRTFILE__' > "$TMPDIR/diag_env.txt"; \
        cat "$TMPDIR/diag_env.txt"; \
        rm -f "$TMPDIR/diag_env.txt"; \
        printf '\\n__PATHVIRT_END__\\n'
        """
        let probe = await runShellProbe(argv: ["exsh", "-c", command])
        let text = probe.transcript
        let ok = probe.errnoValue == 0 &&
            probe.exitStatus == 0 &&
            text.contains("__PATHVIRT_BEGIN__") &&
            text.contains("HOME=") &&
            text.contains("PWD=/home") &&
            text.contains("TMPDIR=") &&
            text.contains("PSCALI_WORKDIR=") &&
            text.contains("DOCS_PWD=/home/Docs") &&
            text.contains("__VIRTFILE__") &&
            text.contains("__PATHVIRT_END__")
        let details = [
            "session id: \(probe.sessionId)",
            "exit status: \(probe.exitStatus.map(String.init) ?? "nil")",
            "errno: \(probe.errnoValue)",
            "transcript:",
            text.isEmpty ? "(empty)" : text
        ].joined(separator: "\n")
        return AppDiagnosticsCheckResult(name: "Path virtualization",
                                         status: ok ? .pass : .fail,
                                         summary: ok
                                             ? "shell environment variables and virtualized paths behaved consistently"
                                             : "shell environment/path virtualization did not behave as expected",
                                         details: details)
    }

    private static func toolBuiltinFallbackCheck() async -> AppDiagnosticsCheckResult {
        let command = "smallclue-help hostname; echo __HOSTNAME_RUN__; hostname"
        let probe = await runShellProbe(argv: ["exsh", "-c", command])
        let hasAppletHelp = probe.transcript.contains("hostname - Show system hostname") ||
            probe.transcript.contains("Usage: hostname")
        let hasRunMarker = probe.transcript.contains("__HOSTNAME_RUN__")
        let hasCommandOutput = probe.transcript
            .split(separator: "\n", omittingEmptySubsequences: true)
            .map { String($0).trimmingCharacters(in: .whitespaces) }
            .contains { line in
                line != "__HOSTNAME_RUN__" &&
                !line.hasPrefix("hostname - ") &&
                !line.hasPrefix("Usage: hostname") &&
                !line.isEmpty
            }
        let ok = probe.errnoValue == 0 &&
            probe.exitStatus == 0 &&
            hasAppletHelp &&
            hasRunMarker &&
            hasCommandOutput
        let details = [
            "session id: \(probe.sessionId)",
            "argv: \(probe.argv.joined(separator: " "))",
            "exit status: \(probe.exitStatus.map(String.init) ?? "nil")",
            "errno: \(probe.errnoValue)",
            "transcript:",
            probe.transcript.isEmpty ? "(empty)" : probe.transcript
        ].joined(separator: "\n")
        return AppDiagnosticsCheckResult(name: "Tool builtin fallback",
                                         status: ok ? .pass : .fail,
                                         summary: ok
                                             ? "smallclue applets resolved and ran through the iOS in-process tool path"
                                             : "smallclue applet resolution or in-process execution did not behave as expected",
                                         details: details)
    }

    private static func terminalCapabilityCheck() async -> AppDiagnosticsCheckResult {
        let command = """
        printf '__TERM_BEGIN__\\n'; \
        printf '\\033[2J\\033[H'; \
        printf '__TERM_CLEAR__\\n'; \
        printf '\\033[?1049h'; \
        printf 'ALTBUF\\n'; \
        printf '\\033[?1049l'; \
        printf '__TERM_END__\\n'
        """
        let probe = await runShellProbe(argv: ["exsh", "-c", command])
        let ok = probe.errnoValue == 0 &&
            probe.exitStatus == 0 &&
            probe.transcript.contains("__TERM_CLEAR__") &&
            probe.transcript.contains("__TERM_END__") &&
            !probe.usingAlternateScreenAtEnd
        let details = [
            "session id: \(probe.sessionId)",
            "exit status: \(probe.exitStatus.map(String.init) ?? "nil")",
            "errno: \(probe.errnoValue)",
            "alternate screen active at end: \(probe.usingAlternateScreenAtEnd)",
            "transcript:",
            probe.transcript.isEmpty ? "(empty)" : probe.transcript
        ].joined(separator: "\n")
        return AppDiagnosticsCheckResult(name: "Terminal capabilities",
                                         status: ok ? .pass : .fail,
                                         summary: ok
                                             ? "clear-screen and alternate-screen sequences completed without leaving stale terminal state"
                                             : "terminal escape-sequence handling did not complete cleanly",
                                         details: details)
    }

    private static func multiSessionIsolationCheck() async -> AppDiagnosticsCheckResult {
        let commandA = "printf '__ISO_A_BEGIN__\\n'; printf 'A_ONLY_1\\nA_ONLY_2\\n'; printf '__ISO_A_END__\\n'"
        let commandB = "printf '__ISO_B_BEGIN__\\n'; printf 'B_ONLY_1\\nB_ONLY_2\\n'; printf '__ISO_B_END__\\n'"
        async let probeA = runShellProbe(argv: ["exsh", "-c", commandA], afterStart: { session in
            session.updateTerminalSize(columns: 100, rows: 26)
            try? await Task.sleep(nanoseconds: 75_000_000)
        })
        async let probeB = runShellProbe(argv: ["exsh", "-c", commandB], afterStart: { session in
            session.updateTerminalSize(columns: 64, rows: 18)
            try? await Task.sleep(nanoseconds: 75_000_000)
        })
        let (a, b) = await (probeA, probeB)

        let okA = a.errnoValue == 0 &&
            a.exitStatus == 0 &&
            a.transcript.contains("__ISO_A_BEGIN__") &&
            a.transcript.contains("__ISO_A_END__") &&
            !a.transcript.contains("__ISO_B_BEGIN__") &&
            a.finalColumns == 100 &&
            a.finalRows == 26
        let okB = b.errnoValue == 0 &&
            b.exitStatus == 0 &&
            b.transcript.contains("__ISO_B_BEGIN__") &&
            b.transcript.contains("__ISO_B_END__") &&
            !b.transcript.contains("__ISO_A_BEGIN__") &&
            b.finalColumns == 64 &&
            b.finalRows == 18
        let details = [
            "session A id: \(a.sessionId) exit=\(a.exitStatus.map(String.init) ?? "nil") errno=\(a.errnoValue) size=\(a.finalColumns)x\(a.finalRows)",
            a.transcript.isEmpty ? "(empty)" : a.transcript,
            "",
            "session B id: \(b.sessionId) exit=\(b.exitStatus.map(String.init) ?? "nil") errno=\(b.errnoValue) size=\(b.finalColumns)x\(b.finalRows)",
            b.transcript.isEmpty ? "(empty)" : b.transcript
        ].joined(separator: "\n")
        return AppDiagnosticsCheckResult(name: "Multi-session isolation",
                                         status: (okA && okB) ? .pass : .fail,
                                         summary: (okA && okB)
                                             ? "concurrent shell sessions kept distinct output and terminal geometry"
                                             : "concurrent shell sessions showed output or geometry leakage",
                                         details: details)
    }

    private static func frontendExecutionSmokeCheck() async -> AppDiagnosticsCheckResult {
        let command = """
        src="/home/Examples/pascal/base/hello"; \
        pascal --dump-bytecode-only --no-cache "$src" >/dev/null 2>&1; \
        status=$?; \
        printf 'FRONTEND_SOURCE=%s\\n' "$src"; \
        printf 'FRONTEND_STATUS=%d\\n' "$status"
        """
        let probe = await runShellProbe(argv: ["exsh", "-c", command], timeout: 12.0)
        let ok = probe.errnoValue == 0 &&
            probe.exitStatus == 0 &&
            probe.transcript.contains("FRONTEND_SOURCE=/home/Examples/pascal/base/hello") &&
            probe.transcript.contains("FRONTEND_STATUS=0")
        let details = [
            "session id: \(probe.sessionId)",
            "exit status: \(probe.exitStatus.map(String.init) ?? "nil")",
            "errno: \(probe.errnoValue)",
            "transcript:",
            probe.transcript.isEmpty ? "(empty)" : probe.transcript
        ].joined(separator: "\n")
        return AppDiagnosticsCheckResult(name: "Frontend execution smoke",
                                         status: ok ? .pass : .fail,
                                         summary: ok
                                             ? "the in-app Pascal frontend compiled a tiny program successfully"
                                             : "the in-app Pascal frontend smoke compile did not complete cleanly",
                                         details: details)
    }

    private static func sanitizeTranscript(_ text: String) -> String {
        let cleaned = text.unicodeScalars.filter { scalar in
            if scalar == "\n" || scalar == "\r" || scalar == "\t" {
                return true
            }
            return scalar.value >= 0x20 && scalar.value != 0x7F
        }
        let normalized = String(String.UnicodeScalarView(cleaned))
            .replacingOccurrences(of: "\r\n", with: "\n")
            .replacingOccurrences(of: "\r", with: "\n")
        let trimmed = normalized.trimmingCharacters(in: .whitespacesAndNewlines)
        if trimmed.count <= 1200 {
            return trimmed
        }
        let endIndex = trimmed.index(trimmed.startIndex, offsetBy: 1200)
        return String(trimmed[..<endIndex]) + "\n...[truncated]..."
    }
}

struct AppDiagnosticsView: View {
    @Environment(\.dismiss) private var dismiss
    @StateObject private var runner = AppDiagnosticsRunner()
    @State private var copiedReport: Bool = false

    var body: some View {
        NavigationView {
            List {
                Section(header: Text("Summary")) {
                    HStack {
                        Text("Last run")
                        Spacer()
                        Text(runner.lastRunDescription)
                            .foregroundColor(.secondary)
                    }
                    if runner.isRunning {
                        HStack(spacing: 12) {
                            ProgressView()
                            Text("Running diagnostics...")
                        }
                    } else if runner.checks.isEmpty {
                        Text("Tap Run Diagnostics to execute the in-app sanity checks.")
                            .foregroundColor(.secondary)
                    }
                }

                if !runner.checks.isEmpty {
                    Section(header: Text("Checks")) {
                        ForEach(runner.checks) { check in
                            VStack(alignment: .leading, spacing: 6) {
                                HStack(spacing: 10) {
                                    Image(systemName: check.status.systemImageName)
                                        .foregroundColor(check.status.color)
                                    Text(check.name)
                                        .font(.headline)
                                }
                                Text(check.summary)
                                    .font(.subheadline)
                                if !check.details.isEmpty {
                                    Text(check.details)
                                        .font(.system(.footnote, design: .monospaced))
                                        .textSelection(.enabled)
                                        .foregroundColor(.secondary)
                                }
                            }
                            .padding(.vertical, 4)
                        }
                    }
                }

                if !runner.report.isEmpty {
                    Section(header: Text("Report")) {
                        Text(runner.report)
                            .font(.system(.footnote, design: .monospaced))
                            .textSelection(.enabled)
                    }
                }
            }
            .navigationTitle("App Diagnostics")
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Close") {
                        dismiss()
                    }
                }
                ToolbarItemGroup(placement: .primaryAction) {
                    Button(copiedReport ? "Copied" : "Copy Report") {
                        runner.copyReportToPasteboard()
                        copiedReport = true
                        DispatchQueue.main.asyncAfter(deadline: .now() + 1.5) {
                            copiedReport = false
                        }
                    }
                    .disabled(runner.report.isEmpty)

                    Button(runner.isRunning ? "Running..." : (runner.checks.isEmpty ? "Run Diagnostics" : "Run Again")) {
                        copiedReport = false
                        runner.run()
                    }
                    .disabled(runner.isRunning)
                }
            }
        }
    }
}
