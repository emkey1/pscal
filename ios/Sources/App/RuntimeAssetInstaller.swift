import Foundation
import Darwin

private enum RuntimePaths {
    static var documentsDirectory: URL {
        FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
    }

    static var examplesWorkspaceDirectory: URL {
        documentsDirectory.appendingPathComponent("Examples", isDirectory: true)
    }

    static var stagedToolRunner: URL {
        documentsDirectory.appendingPathComponent("pscal_tool_runner", isDirectory: false)
    }

    static var workspaceExamplesVersionMarker: URL {
        documentsDirectory.appendingPathComponent(".examples.version", isDirectory: false)
    }

    static var workspaceEtcDirectory: URL {
        documentsDirectory.appendingPathComponent("etc", isDirectory: true)
    }

    static var workspaceEtcVersionMarker: URL {
        documentsDirectory.appendingPathComponent(".etc.version", isDirectory: false)
    }

}

final class RuntimeAssetInstaller {
    static let shared = RuntimeAssetInstaller()

    private let fileManager = FileManager.default
    private var cachedToolRunnerPath: String?
    private let assetsVersion: String = {
        if let build = Bundle.main.object(forInfoDictionaryKey: "CFBundleVersion") as? String, !build.isEmpty {
            return build
        }
        if let short = Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String, !short.isEmpty {
            return short
        }
        return "0"
    }()

    private init() {}

    func prepareWorkspace() {
        guard let bundleRoot = Bundle.main.resourceURL else {
            NSLog("PSCAL iOS: missing bundle resource root; cannot configure runtime paths.")
            return
        }

        installWorkspaceExamplesIfNeeded(bundleRoot: bundleRoot)
        installWorkspaceEtcIfNeeded(bundleRoot: bundleRoot)
        configureRuntimeEnvironment(bundleRoot: bundleRoot)

        let workspacePath = RuntimePaths.documentsDirectory.path
        if !fileManager.changeCurrentDirectoryPath(workspacePath) {
            NSLog("PSCAL iOS: failed to switch working directory to %@", workspacePath)
        }
        setenv("PSCALI_WORKSPACE_ROOT", workspacePath, 1)
    }

    func ensureToolRunnerExecutable() -> String? {
        if let cached = cachedToolRunnerPath {
#if targetEnvironment(simulator)
            if fileManager.isExecutableFile(atPath: cached) {
                return cached
            }
#else
            return cached
#endif
        }

        guard let bundledRunner = Bundle.main.url(forResource: "pscal_tool_runner", withExtension: nil),
              fileManager.fileExists(atPath: bundledRunner.path) else {
            NSLog("PSCAL iOS: missing pscal_tool_runner in bundle; tool builtins will be unavailable.")
            cachedToolRunnerPath = nil
            return nil
        }

#if targetEnvironment(simulator)
        let stagedRunner = RuntimePaths.stagedToolRunner

        do {
            try ensureDocumentsDirectoryExists()
            if fileManager.fileExists(atPath: stagedRunner.path) {
                if fileManager.contentsEqual(atPath: stagedRunner.path, andPath: bundledRunner.path) {
                    try markExecutable(at: stagedRunner)
                    cachedToolRunnerPath = stagedRunner.path
                    return stagedRunner.path
                }
                try fileManager.removeItem(at: stagedRunner)
            }

            try fileManager.copyItem(at: bundledRunner, to: stagedRunner)
            try markExecutable(at: stagedRunner)
            cachedToolRunnerPath = stagedRunner.path
            return stagedRunner.path
        } catch {
            NSLog("PSCAL iOS: failed to stage tool runner: %@", error.localizedDescription)
            cachedToolRunnerPath = nil
            return nil
        }
#else
        cachedToolRunnerPath = bundledRunner.path
        return bundledRunner.path
#endif
    }

    private func installWorkspaceExamplesIfNeeded(bundleRoot: URL) {
        let bundledExamples = bundleRoot.appendingPathComponent("Examples", isDirectory: true)
        guard fileManager.fileExists(atPath: bundledExamples.path) else {
            NSLog("PSCAL iOS: bundle missing Examples directory; skipping copy.")
            return
        }

        let workspaceExamples = RuntimePaths.examplesWorkspaceDirectory
        if needsWorkspaceExamplesRefresh() {
            do {
                if fileManager.fileExists(atPath: workspaceExamples.path) || isSymbolicLink(at: workspaceExamples) {
                    try fileManager.removeItem(at: workspaceExamples)
                }
                try ensureDocumentsDirectoryExists()
                try fileManager.copyItem(at: bundledExamples, to: workspaceExamples)
                try writeWorkspaceExamplesVersionMarker()
                rewritePlaceholders(in: workspaceExamples, installRoot: bundleRoot.path)
                NSLog("PSCAL iOS: refreshed Examples workspace at %@", workspaceExamples.path)
            } catch {
                NSLog("PSCAL iOS: failed to install workspace Examples directory: %@", error.localizedDescription)
            }
        } else {
            rewritePlaceholders(in: workspaceExamples, installRoot: bundleRoot.path)
        }
    }

    private func installWorkspaceEtcIfNeeded(bundleRoot: URL) {
        let bundledEtc = bundleRoot.appendingPathComponent("etc", isDirectory: true)
        guard fileManager.fileExists(atPath: bundledEtc.path) else {
            NSLog("PSCAL iOS: bundle missing etc directory; skipping installation.")
            return
        }

        let workspaceEtc = RuntimePaths.workspaceEtcDirectory
        if needsWorkspaceEtcRefresh() {
            do {
                if fileManager.fileExists(atPath: workspaceEtc.path) || isSymbolicLink(at: workspaceEtc) {
                    try fileManager.removeItem(at: workspaceEtc)
                }
                try ensureDocumentsDirectoryExists()
                try fileManager.copyItem(at: bundledEtc, to: workspaceEtc)
                try writeWorkspaceEtcVersionMarker()
                NSLog("PSCAL iOS: installed etc assets to %@", workspaceEtc.path)
            } catch {
                NSLog("PSCAL iOS: failed to install etc assets: %@", error.localizedDescription)
            }
        }
    }

    private func needsWorkspaceExamplesRefresh() -> Bool {
        let workspaceExamples = RuntimePaths.examplesWorkspaceDirectory
        var isDirectory: ObjCBool = false
        let exists = fileManager.fileExists(atPath: workspaceExamples.path, isDirectory: &isDirectory)
        if !exists {
            return true
        }
        if isSymbolicLink(at: workspaceExamples) {
            return true
        }
        if !isDirectory.boolValue {
            return true
        }
        if directoryIsEmpty(workspaceExamples) {
            return true
        }
        guard let data = try? Data(contentsOf: RuntimePaths.workspaceExamplesVersionMarker),
              let recorded = String(data: data, encoding: .utf8)?
                .trimmingCharacters(in: .whitespacesAndNewlines),
              !recorded.isEmpty else {
            return true
        }
        return recorded != assetsVersion
    }

    private func needsWorkspaceEtcRefresh() -> Bool {
        let workspaceEtc = RuntimePaths.workspaceEtcDirectory
        var isDirectory: ObjCBool = false
        let exists = fileManager.fileExists(atPath: workspaceEtc.path, isDirectory: &isDirectory)
        if !exists || isSymbolicLink(at: workspaceEtc) || !isDirectory.boolValue {
            return true;
        }
        guard let data = try? Data(contentsOf: RuntimePaths.workspaceEtcVersionMarker),
              let recorded = String(data: data, encoding: .utf8)?
                .trimmingCharacters(in: .whitespacesAndNewlines),
              !recorded.isEmpty else {
            return true
        }
        return recorded != assetsVersion
    }

    private func configureRuntimeEnvironment(bundleRoot: URL) {
        let runtimePath = bundleRoot.path
        setenv("PSCALI_INSTALL_ROOT", runtimePath, 1)
        setenv("PSCAL_INSTALL_ROOT", runtimePath, 1)
        setenv("PSCAL_INSTALL_ROOT_RESOLVED", runtimePath, 1)
        setenv("PASCAL_LIB_DIR", bundleRoot.appendingPathComponent("lib/pascal").path, 1)
        setenv("CLIKE_LIB_DIR", bundleRoot.appendingPathComponent("lib/clike").path, 1)
        setenv("REA_LIB_DIR", bundleRoot.appendingPathComponent("lib/rea").path, 1)

        configureReaImportPath(bundleRoot: bundleRoot)

        let workspaceExamplesPath = RuntimePaths.examplesWorkspaceDirectory.path
        if fileManager.fileExists(atPath: workspaceExamplesPath) {
            setenv("PSCAL_EXAMPLES_ROOT", workspaceExamplesPath, 1)
        } else {
            let bundledExamples = bundleRoot.appendingPathComponent("Examples", isDirectory: true)
            if fileManager.fileExists(atPath: bundledExamples.path) {
                setenv("PSCAL_EXAMPLES_ROOT", bundledExamples.path, 1)
            }
        }

        let workspaceEtcPath = RuntimePaths.workspaceEtcDirectory.path
        if fileManager.fileExists(atPath: workspaceEtcPath) {
            setenv("PSCALI_ETC_ROOT", workspaceEtcPath, 1)
        } else {
            let bundledEtc = bundleRoot.appendingPathComponent("etc", isDirectory: true)
            if fileManager.fileExists(atPath: bundledEtc.path) {
                setenv("PSCALI_ETC_ROOT", bundledEtc.path, 1)
            }
        }
    }

    private func configureReaImportPath(bundleRoot: URL) {
        var components: [String] = []
        if let cString = getenv("REA_IMPORT_PATH"), cString.pointee != 0 {
            if let current = String(validatingUTF8: cString), !current.isEmpty {
                components.append(current)
            }
        }

        components.append(bundleRoot.appendingPathComponent("lib/rea").path)

        let joined = components.joined(separator: ":")
        setenv("REA_IMPORT_PATH", joined, 1)
    }

    private func ensureDocumentsDirectoryExists() throws {
        if !fileManager.fileExists(atPath: RuntimePaths.documentsDirectory.path) {
            try fileManager.createDirectory(at: RuntimePaths.documentsDirectory,
                                            withIntermediateDirectories: true)
        }
    }

    private func writeWorkspaceExamplesVersionMarker() throws {
        let data = (assetsVersion + "\n").data(using: .utf8) ?? Data()
        try data.write(to: RuntimePaths.workspaceExamplesVersionMarker, options: [.atomic])
    }

    private func writeWorkspaceEtcVersionMarker() throws {
        let data = (assetsVersion + "\n").data(using: .utf8) ?? Data()
        try data.write(to: RuntimePaths.workspaceEtcVersionMarker, options: [.atomic])
    }

    private func markExecutable(at url: URL) throws {
        try fileManager.setAttributes([.posixPermissions: NSNumber(value: Int16(0o755))],
                                      ofItemAtPath: url.path)
    }

    private func isSymbolicLink(at url: URL) -> Bool {
        return (try? fileManager.destinationOfSymbolicLink(atPath: url.path)) != nil
    }

    private func directoryIsEmpty(_ url: URL) -> Bool {
        guard let enumerator = fileManager.enumerator(atPath: url.path) else {
            return true
        }
        for case let item as String in enumerator {
            if item == ".DS_Store" {
                continue
            }
            return false
        }
        return true
    }

    private func rewritePlaceholders(in directory: URL, installRoot: String) {
        guard let enumerator = fileManager.enumerator(at: directory, includingPropertiesForKeys: [.isRegularFileKey], options: [.skipsHiddenFiles]) else {
            return
        }

        let replacements: [(String, String)] = [
            ("@PSCAL_INSTALL_ROOT@", installRoot),
            ("@PSCAL_INSTALL_ROOT_RESOLVED@", installRoot)
        ]

        for case let fileURL as URL in enumerator {
            do {
                let values = try fileURL.resourceValues(forKeys: [.isRegularFileKey])
                if values.isRegularFile != true {
                    continue
                }
                let original = try String(contentsOf: fileURL, encoding: .utf8)
                var updated = original
                for (token, value) in replacements {
                    if updated.contains(token) {
                        updated = updated.replacingOccurrences(of: token, with: value)
                    }
                }
                if updated != original {
                    try updated.write(to: fileURL, atomically: true, encoding: .utf8)
                }
            } catch {
                continue
            }
        }
    }
}
