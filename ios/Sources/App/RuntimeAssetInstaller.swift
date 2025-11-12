import Foundation
import Darwin

private enum RuntimePaths {
    static var documentsDirectory: URL {
        FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
    }

    static var examplesDirectory: URL {
        documentsDirectory.appendingPathComponent("Examples", isDirectory: true)
    }

    static var versionMarker: URL {
        documentsDirectory.appendingPathComponent(".examples.version", isDirectory: false)
    }
}

final class RuntimeAssetInstaller {
    static let shared = RuntimeAssetInstaller()

    private let fileManager = FileManager.default
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
        if installExamplesIfNeeded() {
            NSLog("PSCAL iOS: staged Examples directory at %@", RuntimePaths.examplesDirectory.path)
        }

        let workspacePath = RuntimePaths.documentsDirectory.path
        if !fileManager.changeCurrentDirectoryPath(workspacePath) {
            NSLog("PSCAL iOS: failed to switch working directory to %@", workspacePath)
        }

        if fileManager.fileExists(atPath: RuntimePaths.examplesDirectory.path) {
            setenv("PSCAL_EXAMPLES_ROOT", RuntimePaths.examplesDirectory.path, 1)
        }
        setenv("PSCALI_WORKSPACE_ROOT", workspacePath, 1)
    }

    @discardableResult
    private func installExamplesIfNeeded() -> Bool {
        guard let bundledExamples = Bundle.main.resourceURL?.appendingPathComponent("Examples", isDirectory: true),
              fileManager.fileExists(atPath: bundledExamples.path) else {
            return false
        }

        guard needsExamplesRefresh() else {
            return false
        }

        do {
            try ensureDocumentsDirectoryExists()
            if fileManager.fileExists(atPath: RuntimePaths.examplesDirectory.path) {
                try fileManager.removeItem(at: RuntimePaths.examplesDirectory)
            }
            try fileManager.copyItem(at: bundledExamples, to: RuntimePaths.examplesDirectory)
            try writeVersionMarker()
            return true
        } catch {
            NSLog("PSCAL iOS: failed to prepare Examples bundle: %@", error.localizedDescription)
            return false
        }
    }

    private func needsExamplesRefresh() -> Bool {
        guard fileManager.fileExists(atPath: RuntimePaths.examplesDirectory.path) else {
            return true
        }
        guard let data = try? Data(contentsOf: RuntimePaths.versionMarker),
              let recorded = String(data: data, encoding: .utf8)?
                .trimmingCharacters(in: .whitespacesAndNewlines),
              !recorded.isEmpty else {
            return true
        }
        return recorded != assetsVersion
    }

    private func ensureDocumentsDirectoryExists() throws {
        if !fileManager.fileExists(atPath: RuntimePaths.documentsDirectory.path) {
            try fileManager.createDirectory(at: RuntimePaths.documentsDirectory,
                                            withIntermediateDirectories: true)
        }
    }

    private func writeVersionMarker() throws {
        let data = (assetsVersion + "\n").data(using: .utf8) ?? Data()
        try data.write(to: RuntimePaths.versionMarker, options: [.atomic])
    }
}
