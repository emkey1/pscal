import Foundation
import Darwin

private enum RuntimePaths {
    static var documentsDirectory: URL {
        FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
    }

    static var runtimeRootDirectory: URL {
        documentsDirectory
    }

    static var examplesWorkspaceDirectory: URL {
        homeDirectory.appendingPathComponent("Examples", isDirectory: true)
    }

    static var docsWorkspaceDirectory: URL {
        homeDirectory.appendingPathComponent("Docs", isDirectory: true)
    }

    static var stagedToolRunner: URL {
        documentsDirectory.appendingPathComponent("pscal_tool_runner", isDirectory: false)
    }

    static var workspaceExamplesVersionMarker: URL {
        homeDirectory.appendingPathComponent(".examples.version", isDirectory: false)
    }

    static var workspaceDocsVersionMarker: URL {
        homeDirectory.appendingPathComponent(".docs.version", isDirectory: false)
    }

    static var tmpDirectory: URL {
        documentsDirectory.appendingPathComponent("tmp", isDirectory: true)
    }

    static var homeDirectory: URL {
        documentsDirectory.appendingPathComponent("home", isDirectory: true)
    }

    static var workspaceEtcDirectory: URL {
        documentsDirectory.appendingPathComponent("etc", isDirectory: true)
    }

    static var workspaceEtcVersionMarker: URL {
        documentsDirectory.appendingPathComponent(".etc.version", isDirectory: false)
    }

    static var legacySysfilesDirectory: URL {
        documentsDirectory.appendingPathComponent("sysfiles", isDirectory: true)
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

        migrateLegacySysfilesIfNeeded()

        do {
            try ensureWorkspaceDirectoriesExist()
        } catch {
            NSLog("PSCAL iOS: failed to initialize workspace directories: %@", error.localizedDescription)
        }

        installWorkspaceExamplesIfNeeded(bundleRoot: bundleRoot)
        installWorkspaceDocsIfNeeded(bundleRoot: bundleRoot)
        installWorkspaceEtcIfNeeded(bundleRoot: bundleRoot)
        configureRuntimeEnvironment(bundleRoot: bundleRoot)

        let workspacePath = RuntimePaths.documentsDirectory.path
        if !fileManager.changeCurrentDirectoryPath(workspacePath) {
            NSLog("PSCAL iOS: failed to switch working directory to %@", workspacePath)
        }
        setenv("PSCALI_WORKSPACE_ROOT", workspacePath, 1)

        installSkelHomeIfNeeded()
    }

    func ensureToolRunnerExecutable() -> String? {
        if let cached = cachedToolRunnerPath {
            if fileManager.isExecutableFile(atPath: cached) {
                return cached
            }
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
        // On device, execute directly from the signed bundle location.
        if fileManager.isExecutableFile(atPath: bundledRunner.path) {
            cachedToolRunnerPath = bundledRunner.path
            return bundledRunner.path
        }
        NSLog("PSCAL iOS: bundled pscal_tool_runner is not executable at %@", bundledRunner.path)
        cachedToolRunnerPath = nil
        return nil
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
                try ensureWorkspaceDirectoriesExist()
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

    private func installWorkspaceDocsIfNeeded(bundleRoot: URL) {
        let bundledDocs = bundleRoot.appendingPathComponent("Docs", isDirectory: true)
        guard fileManager.fileExists(atPath: bundledDocs.path) else {
            NSLog("PSCAL iOS: bundle missing Docs directory; skipping copy.")
            return
        }

        let workspaceDocs = RuntimePaths.docsWorkspaceDirectory
        if needsWorkspaceDocsRefresh() {
            do {
                if fileManager.fileExists(atPath: workspaceDocs.path) || isSymbolicLink(at: workspaceDocs) {
                    try fileManager.removeItem(at: workspaceDocs)
                }
                try ensureWorkspaceDirectoriesExist()
                try fileManager.copyItem(at: bundledDocs, to: workspaceDocs)
                try writeWorkspaceDocsVersionMarker()
                rewritePlaceholders(in: workspaceDocs, installRoot: bundleRoot.path)
                NSLog("PSCAL iOS: refreshed Docs workspace at %@", workspaceDocs.path)
            } catch {
                NSLog("PSCAL iOS: failed to install Docs directory: %@", error.localizedDescription)
            }
        } else {
            rewritePlaceholders(in: workspaceDocs, installRoot: bundleRoot.path)
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
                try ensureWorkspaceDirectoriesExist()
                try fileManager.copyItem(at: bundledEtc, to: workspaceEtc)
                try writeWorkspaceEtcVersionMarker()
                NSLog("PSCAL iOS: installed etc assets to %@", workspaceEtc.path)
            } catch {
                NSLog("PSCAL iOS: failed to install etc assets: %@", error.localizedDescription)
            }
        }

        ensureEtcSubdirectoryNamed("ssh", bundleRoot: bundleRoot)
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

    private func ensureEtcSubdirectoryNamed(_ name: String, bundleRoot: URL) {
        let bundledEtc = bundleRoot.appendingPathComponent("etc", isDirectory: true)
        let bundleSubdirectory = bundledEtc.appendingPathComponent(name, isDirectory: true)
        guard fileManager.fileExists(atPath: bundleSubdirectory.path) else {
            return
        }
        let workspaceSubdirectory = RuntimePaths.workspaceEtcDirectory.appendingPathComponent(name, isDirectory: true)
        do {
            try ensureWorkspaceDirectoriesExist()
            var isDirectory: ObjCBool = false
            let exists = fileManager.fileExists(atPath: workspaceSubdirectory.path, isDirectory: &isDirectory)
            if !exists || !isDirectory.boolValue {
                if exists {
                    try fileManager.removeItem(at: workspaceSubdirectory)
                }
                try fileManager.createDirectory(at: workspaceSubdirectory, withIntermediateDirectories: true)
            }
            try copyMissingItems(from: bundleSubdirectory, to: workspaceSubdirectory)
        } catch {
            NSLog("PSCAL iOS: failed to sync etc/%@ assets: %@", name, error.localizedDescription)
        }
    }

    private func copyMissingItems(from source: URL, to destination: URL) throws {
        let entries = try fileManager.contentsOfDirectory(atPath: source.path)
        for entry in entries where entry != ".DS_Store" {
            let sourceURL = source.appendingPathComponent(entry)
            let destinationURL = destination.appendingPathComponent(entry)
            var isDirectory: ObjCBool = false
            guard fileManager.fileExists(atPath: sourceURL.path, isDirectory: &isDirectory) else {
                continue
            }
            if isDirectory.boolValue {
                if !fileManager.fileExists(atPath: destinationURL.path, isDirectory: nil) {
                    try fileManager.createDirectory(at: destinationURL, withIntermediateDirectories: true)
                }
                try copyMissingItems(from: sourceURL, to: destinationURL)
            } else if !fileManager.fileExists(atPath: destinationURL.path) {
                try fileManager.copyItem(at: sourceURL, to: destinationURL)
            }
        }
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

        let workspaceDocsPath = RuntimePaths.docsWorkspaceDirectory.path
        if fileManager.fileExists(atPath: workspaceDocsPath) {
            setenv("PSCALI_DOCS_ROOT", workspaceDocsPath, 1)
        } else {
            let bundledDocs = bundleRoot.appendingPathComponent("Docs", isDirectory: true)
            if fileManager.fileExists(atPath: bundledDocs.path) {
                setenv("PSCALI_DOCS_ROOT", bundledDocs.path, 1)
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

        let runtimeRoot = RuntimePaths.runtimeRootDirectory.path
        setenv("PSCALI_SYSFILES_ROOT", runtimeRoot, 1)
        let tmpPath = RuntimePaths.tmpDirectory.path
        setenv("TMPDIR", tmpPath, 1)
        setenv("SESSIONPATH", "\(tmpPath):~:.", 1)
        setenv("HOME", RuntimePaths.homeDirectory.path, 1)
        setenv("TERM", "xterm-256color", 1)
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

    private func installSkelHomeIfNeeded() {
        guard let bundleRoot = Bundle.main.resourceURL else {
            return
        }
        let skel = bundleRoot.appendingPathComponent("etc/skel", isDirectory: true)
        let home = RuntimePaths.homeDirectory
        guard fileManager.fileExists(atPath: skel.path) else {
            return
        }
        do {
            let entries = try fileManager.contentsOfDirectory(atPath: skel.path)
            for entry in entries where entry != ".DS_Store" {
                let source = skel.appendingPathComponent(entry)
                let destination = home.appendingPathComponent(entry)
                syncSkelEntry(from: source, to: destination, isExshrc: (entry == ".exshrc"))
            }
        } catch {
            NSLog("PSCAL iOS: failed to enumerate skel dir: %@", error.localizedDescription)
        }
    }

    private func syncSkelEntry(from source: URL, to destination: URL, isExshrc: Bool) {
        guard let data = try? Data(contentsOf: source),
              let newContents = String(data: data, encoding: .utf8) else {
            return
        }
        if !fileManager.fileExists(atPath: destination.path) {
            do {
                try newContents.write(to: destination, atomically: true, encoding: .utf8)
            } catch {
                NSLog("PSCAL iOS: failed to write .exshrc: %@", error.localizedDescription)
            }
            return
        }
        guard isExshrc,
              let oldData = try? Data(contentsOf: destination),
              let oldContents = String(data: oldData, encoding: .utf8) else {
            do {
                try newContents.write(to: destination, atomically: true, encoding: .utf8)
            } catch {
                NSLog("PSCAL iOS: failed to update %@: %@", destination.lastPathComponent, error.localizedDescription)
            }
            return
        }
        let containsShortPrompt = oldContents.contains("\\W")
        let containsFullPrompt = oldContents.contains("\\w")
        if !containsShortPrompt && containsFullPrompt {
            do {
                try newContents.write(to: destination, atomically: true, encoding: .utf8)
            } catch {
                NSLog("PSCAL iOS: failed to update .exshrc: %@", error.localizedDescription)
            }
        }
    }

    private func writeWorkspaceExamplesVersionMarker() throws {
        let data = (assetsVersion + "\n").data(using: .utf8) ?? Data()
        try data.write(to: RuntimePaths.workspaceExamplesVersionMarker, options: [.atomic])
    }

    private func needsWorkspaceDocsRefresh() -> Bool {
        let workspaceDocs = RuntimePaths.docsWorkspaceDirectory
        var isDirectory: ObjCBool = false
        let exists = fileManager.fileExists(atPath: workspaceDocs.path, isDirectory: &isDirectory)
        if !exists {
            return true
        }
        if isSymbolicLink(at: workspaceDocs) {
            return true
        }
        if !isDirectory.boolValue || directoryIsEmpty(workspaceDocs) {
            return true
        }
        guard let data = try? Data(contentsOf: RuntimePaths.workspaceDocsVersionMarker),
              let recorded = String(data: data, encoding: .utf8)?
                .trimmingCharacters(in: .whitespacesAndNewlines),
              !recorded.isEmpty else {
            return true
        }
        return recorded != assetsVersion
    }

    private func writeWorkspaceDocsVersionMarker() throws {
        let data = (assetsVersion + "\n").data(using: .utf8) ?? Data()
        try data.write(to: RuntimePaths.workspaceDocsVersionMarker, options: [.atomic])
    }

    private func writeWorkspaceEtcVersionMarker() throws {
        try ensureWorkspaceDirectoriesExist()
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

    private func ensureWorkspaceDirectoriesExist() throws {
        try ensureDocumentsDirectoryExists()
        let requiredDirectories = [
            RuntimePaths.homeDirectory,
            RuntimePaths.tmpDirectory
        ]
        for directory in requiredDirectories {
            if !fileManager.fileExists(atPath: directory.path) {
                try fileManager.createDirectory(at: directory, withIntermediateDirectories: true)
            }
        }
    }

    private func migrateLegacySysfilesIfNeeded() {
        let legacyRoot = RuntimePaths.legacySysfilesDirectory
        var isDirectory: ObjCBool = false
        guard fileManager.fileExists(atPath: legacyRoot.path, isDirectory: &isDirectory), isDirectory.boolValue else {
            return
        }

        do {
            let entries = try fileManager.contentsOfDirectory(atPath: legacyRoot.path)
            if entries.isEmpty {
                try fileManager.removeItem(at: legacyRoot)
                return
            }

            try ensureDocumentsDirectoryExists()
            for entry in entries {
                let source = legacyRoot.appendingPathComponent(entry)
                let destination = RuntimePaths.documentsDirectory.appendingPathComponent(entry)
                if fileManager.fileExists(atPath: destination.path) {
                    continue
                }
                try fileManager.moveItem(at: source, to: destination)
            }
            try fileManager.removeItem(at: legacyRoot)
            NSLog("PSCAL iOS: migrated legacy sysfiles hierarchy into %@", RuntimePaths.documentsDirectory.path)
        } catch {
            NSLog("PSCAL iOS: failed to migrate legacy sysfiles content: %@", error.localizedDescription)
        }
    }
}
