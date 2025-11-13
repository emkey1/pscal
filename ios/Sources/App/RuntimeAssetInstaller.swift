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
        installWorkspaceExamplesIfNeeded()
        configureRuntimeEnvironment()

        let workspacePath = RuntimePaths.documentsDirectory.path
        if !fileManager.changeCurrentDirectoryPath(workspacePath) {
            NSLog("PSCAL iOS: failed to switch working directory to %@", workspacePath)
        }
        setenv("PSCALI_WORKSPACE_ROOT", workspacePath, 1)
    }

    func ensureToolRunnerExecutable() -> String? {
        if let cached = cachedToolRunnerPath,
           fileManager.isExecutableFile(atPath: cached) {
            return cached
        }

        guard let bundledRunner = Bundle.main.url(forResource: "pscal_tool_runner", withExtension: nil),
              fileManager.fileExists(atPath: bundledRunner.path) else {
            NSLog("PSCAL iOS: missing pscal_tool_runner in bundle; tool builtins will be unavailable.")
            cachedToolRunnerPath = nil
            return nil
        }

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
    }

    private func installWorkspaceExamplesIfNeeded() {
        guard let bundledExamples = Bundle.main.resourceURL?.appendingPathComponent("Examples", isDirectory: true),
              fileManager.fileExists(atPath: bundledExamples.path) else {
            NSLog("PSCAL iOS: bundle missing Examples directory; skipping copy.")
            return
        }

        guard needsWorkspaceExamplesRefresh() else {
            return
        }

        let workspaceExamples = RuntimePaths.examplesWorkspaceDirectory
        do {
            if fileManager.fileExists(atPath: workspaceExamples.path) || isSymbolicLink(at: workspaceExamples) {
                try fileManager.removeItem(at: workspaceExamples)
            }
            try ensureDocumentsDirectoryExists()
            try fileManager.copyItem(at: bundledExamples, to: workspaceExamples)
            try writeWorkspaceExamplesVersionMarker()
            NSLog("PSCAL iOS: refreshed Examples workspace at %@", workspaceExamples.path)
        } catch {
            NSLog("PSCAL iOS: failed to install workspace Examples directory: %@", error.localizedDescription)
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

    private func configureRuntimeEnvironment() {
        guard let bundleRoot = Bundle.main.resourceURL else {
            NSLog("PSCAL iOS: missing bundle resource root; cannot configure runtime paths.")
            return
        }

        let runtimePath = bundleRoot.path
        setenv("PSCALI_INSTALL_ROOT", runtimePath, 1)
        setenv("PSCAL_INSTALL_ROOT", runtimePath, 1)
        setenv("PSCAL_INSTALL_ROOT_RESOLVED", runtimePath, 1)
        setenv("PASCAL_LIB_DIR", bundleRoot.appendingPathComponent("lib/pascal").path, 1)
        setenv("CLIKE_LIB_DIR", bundleRoot.appendingPathComponent("lib/clike").path, 1)
        setenv("REA_LIB_DIR", bundleRoot.appendingPathComponent("lib/rea").path, 1)

        let workspaceExamplesPath = RuntimePaths.examplesWorkspaceDirectory.path
        if fileManager.fileExists(atPath: workspaceExamplesPath) {
            setenv("PSCAL_EXAMPLES_ROOT", workspaceExamplesPath, 1)
        } else {
            let bundledExamples = bundleRoot.appendingPathComponent("Examples", isDirectory: true)
            if fileManager.fileExists(atPath: bundledExamples.path) {
                setenv("PSCAL_EXAMPLES_ROOT", bundledExamples.path, 1)
            }
        }
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
}
