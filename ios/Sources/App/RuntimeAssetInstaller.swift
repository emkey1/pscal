import Foundation
import Darwin
import Compression

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

    // Workspace root == documentsDirectory; keep bin/src there (not under home/)
    static var workspaceBinDirectory: URL {
        documentsDirectory.appendingPathComponent("bin", isDirectory: true)
    }

    static var workspaceBinVersionMarker: URL {
        documentsDirectory.appendingPathComponent(".bin.version", isDirectory: false)
    }

    static var workspaceSrcCompilerDirectory: URL {
        documentsDirectory.appendingPathComponent("src/compiler", isDirectory: true)
    }

    static var workspaceSrcCoreDirectory: URL {
        documentsDirectory.appendingPathComponent("src/core", isDirectory: true)
    }

    static var workspaceSrcVersionMarker: URL {
        documentsDirectory.appendingPathComponent(".src.version", isDirectory: false)
    }

    static var legacySysfilesDirectory: URL {
        documentsDirectory.appendingPathComponent("sysfiles", isDirectory: true)
    }

    static var sandboxVarHtdocsDirectory: URL {
        documentsDirectory.appendingPathComponent("var/htdocs", isDirectory: true)
    }
}

final class RuntimeAssetInstaller {
    static let shared = RuntimeAssetInstaller()

    private let fileManager = FileManager.default
    private var cachedToolRunnerPath: String?
    private var skelHomeInstalled: Bool = false
    private let skelInstallLock = NSLock()
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

    private func decompressDeflate(_ data: Data) -> Data? {
        var stream = compression_stream(dst_ptr: UnsafeMutablePointer<UInt8>(bitPattern: 0)!,
                                        dst_size: 0,
                                        src_ptr: UnsafePointer<UInt8>(bitPattern: 0)!,
                                        src_size: 0,
                                        state: nil)
        var status = compression_stream_init(&stream, COMPRESSION_STREAM_DECODE, COMPRESSION_ZLIB)
        guard status != COMPRESSION_STATUS_ERROR else { return nil }
        defer { compression_stream_destroy(&stream) }

        let bufferSize = 64 * 1024
        let dstBuffer = UnsafeMutablePointer<UInt8>.allocate(capacity: bufferSize)
        defer { dstBuffer.deallocate() }

        var output = Data()
        data.withUnsafeBytes { rawBuffer in
            guard let base = rawBuffer.bindMemory(to: UInt8.self).baseAddress else { return }
            stream.src_ptr = base
            stream.src_size = data.count

            repeat {
                stream.dst_ptr = dstBuffer
                stream.dst_size = bufferSize
                status = compression_stream_process(&stream, 0)
                let produced = bufferSize - stream.dst_size
                if produced > 0 {
                    output.append(dstBuffer, count: produced)
                }
            } while status == COMPRESSION_STATUS_OK
        }

        return status == COMPRESSION_STATUS_END ? output : nil
    }

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
        installWorkspaceBinIfNeeded(bundleRoot: bundleRoot)
        installWorkspaceSrcIfNeeded(bundleRoot: bundleRoot)
        stageSimpleWebServerAssets(bundleRoot: bundleRoot)
        configureRuntimeEnvironment(bundleRoot: bundleRoot)

        let workspacePath = RuntimePaths.documentsDirectory.path
        if !fileManager.changeCurrentDirectoryPath(workspacePath) {
            NSLog("PSCAL iOS: failed to switch working directory to %@", workspacePath)
        }
        setenv("PSCALI_WORKSPACE_ROOT", workspacePath, 1)

        // Only seed the skeleton home once per app launch; subsequent tabs
        // should respect user edits/removals of ~/.exshrc and other dotfiles.
        var shouldInstallSkel = false
        skelInstallLock.lock()
        if !skelHomeInstalled {
            skelHomeInstalled = true
            shouldInstallSkel = true
        }
        skelInstallLock.unlock()
        if shouldInstallSkel {
            installSkelHomeIfNeeded()
        }
    }

    func ensureToolRunnerExecutable() -> String? {
        if let cached = cachedToolRunnerPath {
            if fileManager.isExecutableFile(atPath: cached) {
                return cached
            }
        }

        guard let bundledRunner = Bundle.main.url(forResource: "pscal_tool_runner", withExtension: nil),
              fileManager.fileExists(atPath: bundledRunner.path) else {
            //NSLog("PSCAL iOS: missing pscal_tool_runner in bundle; tool builtins will be unavailable.")
            cachedToolRunnerPath = nil
            return nil
        }

        let deflated = Bundle.main.url(forResource: "pscal_tool_runner", withExtension: "deflate")
        let stagedRunner = RuntimePaths.stagedToolRunner

        let stageRunner: (Data) -> String? = { data in
            guard let decompressed = self.decompressDeflate(data) else {
                NSLog("PSCAL iOS: failed to decompress tool runner payload.")
                return nil
            }
            do {
                try self.ensureDocumentsDirectoryExists()
                if self.fileManager.fileExists(atPath: stagedRunner.path) {
                    try self.fileManager.removeItem(at: stagedRunner)
                }
                try decompressed.write(to: stagedRunner, options: .atomic)
                try self.markExecutable(at: stagedRunner)
                self.cachedToolRunnerPath = stagedRunner.path
                return stagedRunner.path
            } catch {
                NSLog("PSCAL iOS: failed to stage tool runner: %@", error.localizedDescription)
                self.cachedToolRunnerPath = nil
                return nil
            }
        }

        if let deflated, let data = try? Data(contentsOf: deflated) {
            if let path = stageRunner(data) {
                return path
            }
        }

        // Fallback: if an old-style raw runner is present in the bundle, stage it.
        if fileManager.isExecutableFile(atPath: bundledRunner.path),
           let rawData = try? Data(contentsOf: bundledRunner) {
            return stageRunner(rawData)
        }

        NSLog("PSCAL iOS: missing pscal_tool_runner payload in bundle.")
        cachedToolRunnerPath = nil
        return nil
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

        ensureLicensesFromBundle(bundleRoot: bundleRoot, workspaceDocs: workspaceDocs)
    }

    private func ensureLicensesFromBundle(bundleRoot: URL, workspaceDocs: URL) {
        let bundledLicenses = bundleRoot.appendingPathComponent("Docs/Licenses", isDirectory: true)
        let destLicenses = workspaceDocs.appendingPathComponent("Licenses", isDirectory: true)
        var sources: [URL] = []
        if fileManager.fileExists(atPath: bundledLicenses.path) {
            sources.append(contentsOf: (try? fileManager.contentsOfDirectory(at: bundledLicenses, includingPropertiesForKeys: nil)) ?? [])
        }
        if sources.isEmpty {
            // Fallback: look for flat license files in the bundle root.
            let fallbackNames = ["pscal_LICENSE.txt", "openssl_LICENSE.txt", "curl_LICENSE.txt", "sdl_LICENSE.txt", "nextvi_LICENSE.txt", "openssh_LICENSE.txt", "yyjson_LICENSE.txt", "hterm_LICENSE.txt"]
            for name in fallbackNames {
                let candidate = bundleRoot.appendingPathComponent(name)
                if fileManager.fileExists(atPath: candidate.path) {
                    sources.append(candidate)
                }
            }
            if sources.isEmpty {
                // Last resort: create the directory so the help command has a place to look, but avoid noisy logs.
                try? fileManager.createDirectory(at: destLicenses, withIntermediateDirectories: true)
                return
            }
        }
        do {
            if !fileManager.fileExists(atPath: destLicenses.path) || isSymbolicLink(at: destLicenses) {
                try fileManager.createDirectory(at: destLicenses, withIntermediateDirectories: true)
            }
            for src in sources {
                let dst = destLicenses.appendingPathComponent(src.lastPathComponent)
                if !fileManager.fileExists(atPath: dst.path) {
                    try fileManager.copyItem(at: src, to: dst)
                }
            }
        } catch {
            NSLog("PSCAL iOS: failed to install license files: %@", error.localizedDescription)
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

        ensureEtcFileNamed("passwd", bundleRoot: bundleRoot)
        ensureEtcFileNamed("group", bundleRoot: bundleRoot)
        // Ensure word lists are always present even if workspace/etc already exists.
        ensureEtcFileNamed("words", bundleRoot: bundleRoot)
        ensureEtcFileNamed("words.short", bundleRoot: bundleRoot)
        ensureEtcFileNamed("words.many", bundleRoot: bundleRoot)
        ensureEtcSubdirectoryNamed("ssh", bundleRoot: bundleRoot)
    }

    private func installWorkspaceBinIfNeeded(bundleRoot: URL) {
        let bundledBin = bundleRoot.appendingPathComponent("bin", isDirectory: true)
        let workspaceBin = RuntimePaths.workspaceBinDirectory
        if needsWorkspaceBinRefresh() {
            do {
                // Clean slate
                if fileManager.fileExists(atPath: workspaceBin.path) || isSymbolicLink(at: workspaceBin) {
                    NSLog("PSCAL iOS: removing stale bin at %@", workspaceBin.path)
                    try fileManager.removeItem(at: workspaceBin)
                }
                try ensureDocumentsDirectoryExists()
                migrateLegacyBinIfNeeded()

                var copied = false
                if fileManager.fileExists(atPath: bundledBin.path) {
                    NSLog("PSCAL iOS: copying bundled bin contents from %@", bundledBin.path)
                    try fileManager.createDirectory(at: workspaceBin, withIntermediateDirectories: true)
                    let items = try fileManager.contentsOfDirectory(atPath: bundledBin.path)
                    for item in items {
                        let src = bundledBin.appendingPathComponent(item)
                        let dst = workspaceBin.appendingPathComponent(item)
                        if fileManager.fileExists(atPath: dst.path) || isSymbolicLink(at: dst) {
                            try fileManager.removeItem(at: dst)
                        }
                        try fileManager.copyItem(at: src, to: dst)
                    }
                    copied = true
                }

                                // Always rewrite wrapper/source to ensure correctness
                try fileManager.createDirectory(at: workspaceBin, withIntermediateDirectories: true)
                let tinyWrapper = workspaceBin.appendingPathComponent("tiny", isDirectory: false)
                let tinySource = workspaceBin.appendingPathComponent("tiny.clike", isDirectory: false)
                try embeddedTinyWrapper.write(to: tinyWrapper, atomically: true, encoding: .utf8)
                try embeddedTinySource.write(to: tinySource, atomically: true, encoding: .utf8)

                if fileManager.fileExists(atPath: tinyWrapper.path) {
                    try markExecutable(at: tinyWrapper)
                } else {
                    NSLog("PSCAL iOS: tiny wrapper not found after install at %@", tinyWrapper.path)
                }
                try writeWorkspaceBinVersionMarker()
                NSLog("PSCAL iOS: installed bin assets to %@", workspaceBin.path)
            } catch {
                NSLog("PSCAL iOS: failed to install bin assets: %@", error.localizedDescription)
            }
        }
    }

    private func installWorkspaceSrcIfNeeded(bundleRoot: URL) {
        let bundledCompiler = bundleRoot.appendingPathComponent("src/compiler", isDirectory: true)
        let bundledCore = bundleRoot.appendingPathComponent("src/core", isDirectory: true)

        let workspaceCompiler = RuntimePaths.workspaceSrcCompilerDirectory
        let workspaceCore = RuntimePaths.workspaceSrcCoreDirectory
        if needsWorkspaceSrcRefresh() {
            do {
                if fileManager.fileExists(atPath: workspaceCompiler.path) || isSymbolicLink(at: workspaceCompiler) {
                    NSLog("PSCAL iOS: removing stale src/compiler at %@", workspaceCompiler.path)
                    try fileManager.removeItem(at: workspaceCompiler)
                }
                if fileManager.fileExists(atPath: workspaceCore.path) || isSymbolicLink(at: workspaceCore) {
                    NSLog("PSCAL iOS: removing stale src/core at %@", workspaceCore.path)
                    try fileManager.removeItem(at: workspaceCore)
                }
                try ensureWorkspaceDirectoriesExist()
                migrateLegacySrcIfNeeded()
                try fileManager.createDirectory(at: workspaceCompiler, withIntermediateDirectories: true)
                try fileManager.createDirectory(at: workspaceCore, withIntermediateDirectories: true)

                if fileManager.fileExists(atPath: bundledCompiler.path) {
                    NSLog("PSCAL iOS: copying bundled bytecode.h from %@", bundledCompiler.path)
                    try fileManager.copyItem(at: bundledCompiler.appendingPathComponent("bytecode.h"),
                                             to: workspaceCompiler.appendingPathComponent("bytecode.h"))
                } else {
                    NSLog("PSCAL iOS: bundled bytecode.h missing; writing embedded header")
                    try embeddedBytecodeHeader.write(to: workspaceCompiler.appendingPathComponent("bytecode.h"), atomically: true, encoding: .utf8)
                }
                if fileManager.fileExists(atPath: bundledCore.path) {
                    NSLog("PSCAL iOS: copying bundled version.h from %@", bundledCore.path)
                    try fileManager.copyItem(at: bundledCore.appendingPathComponent("version.h"),
                                             to: workspaceCore.appendingPathComponent("version.h"))
                } else {
                    NSLog("PSCAL iOS: bundled version.h missing; writing embedded header")
                    try embeddedVersionHeader.write(to: workspaceCore.appendingPathComponent("version.h"), atomically: true, encoding: .utf8)
                }

                try writeWorkspaceSrcVersionMarker()
                NSLog("PSCAL iOS: installed tiny header assets to %@", workspaceCompiler.deletingLastPathComponent().path)
            } catch {
                NSLog("PSCAL iOS: failed to install tiny headers: %@", error.localizedDescription)
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
        if missingCriticalExamples(at: workspaceExamples) {
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

    private func missingCriticalExamples(at root: URL) -> Bool {
        // At minimum we expect the simple_web_server example to be present.
        let clikeServer = root.appendingPathComponent("clike/base/simple_web_server", isDirectory: false)
        return !fileManager.fileExists(atPath: clikeServer.path)
    }

    private func needsWorkspaceBinRefresh() -> Bool {
        let binDir = RuntimePaths.workspaceBinDirectory
        var isDirectory: ObjCBool = false
        if !fileManager.fileExists(atPath: binDir.path, isDirectory: &isDirectory) || !isDirectory.boolValue {
            return true
        }
        let tiny = binDir.appendingPathComponent("tiny")
        if !fileManager.isExecutableFile(atPath: tiny.path) {
            return true
        }
        guard let data = try? Data(contentsOf: RuntimePaths.workspaceBinVersionMarker),
              let recorded = String(data: data, encoding: .utf8)?
                .trimmingCharacters(in: .whitespacesAndNewlines),
              !recorded.isEmpty else {
            return true
        }
        return recorded != assetsVersion
    }

    private func needsWorkspaceSrcRefresh() -> Bool {
        let compiler = RuntimePaths.workspaceSrcCompilerDirectory.appendingPathComponent("bytecode.h")
        let core = RuntimePaths.workspaceSrcCoreDirectory.appendingPathComponent("version.h")
        if !fileManager.fileExists(atPath: compiler.path) || !fileManager.fileExists(atPath: core.path) {
            return true
        }
        guard let data = try? Data(contentsOf: RuntimePaths.workspaceSrcVersionMarker),
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

    private func ensureEtcFileNamed(_ name: String, bundleRoot: URL) {
        let bundledEtc = bundleRoot.appendingPathComponent("etc", isDirectory: true)
        let bundleFile = bundledEtc.appendingPathComponent(name, isDirectory: false)
        guard fileManager.fileExists(atPath: bundleFile.path) else {
            return
        }
        let workspaceFile = RuntimePaths.workspaceEtcDirectory.appendingPathComponent(name, isDirectory: false)
        do {
            try ensureWorkspaceDirectoriesExist()
            if !fileManager.fileExists(atPath: workspaceFile.path) {
                try fileManager.copyItem(at: bundleFile, to: workspaceFile)
            }
        } catch {
            NSLog("PSCAL iOS: failed to ensure etc/%@: %@", name, error.localizedDescription)
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

    private func stageSimpleWebServerAssets(bundleRoot: URL) {
#if os(iOS)
        let bundledHtdocs = bundleRoot.appendingPathComponent("lib/misc/simple_web_server/htdocs", isDirectory: true)
        guard fileManager.fileExists(atPath: bundledHtdocs.path) else { return }
        let targets: [URL] = [
            RuntimePaths.sandboxVarHtdocsDirectory,
            RuntimePaths.homeDirectory.appendingPathComponent("lib/misc/simple_web_server/htdocs", isDirectory: true)
        ]
        let items = (try? fileManager.contentsOfDirectory(at: bundledHtdocs, includingPropertiesForKeys: nil)) ?? []
        for target in targets {
            do {
                try fileManager.createDirectory(at: target, withIntermediateDirectories: true)
                for item in items {
                    let dst = target.appendingPathComponent(item.lastPathComponent)
                    if !fileManager.fileExists(atPath: dst.path) {
                        try fileManager.copyItem(at: item, to: dst)
                    }
                }
            } catch {
                NSLog("PSCAL iOS: failed to stage simple_web_server assets to %@: %@", target.path, error.localizedDescription)
            }
        }
#endif
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
            setenv("PSCALI_WORDS_PATH", (workspaceEtcPath as NSString).appendingPathComponent("words"), 1)
        } else {
            let bundledEtc = bundleRoot.appendingPathComponent("etc", isDirectory: true)
            if fileManager.fileExists(atPath: bundledEtc.path) {
                setenv("PSCALI_ETC_ROOT", bundledEtc.path, 1)
                setenv("PSCALI_WORDS_PATH", bundledEtc.appendingPathComponent("words").path, 1)
            }
        }
        if let etcRootCString = getenv("PSCALI_ETC_ROOT"),
           let etcRoot = String(validatingUTF8: etcRootCString), !etcRoot.isEmpty {
            let termcapPath = (etcRoot as NSString).appendingPathComponent("termcap")
            setenv("TERMCAP", termcapPath, 1)
            let terminfoPath = (etcRoot as NSString).appendingPathComponent("terminfo")
            setenv("TERMINFO", terminfoPath, 1)
        }

        let runtimeRoot = RuntimePaths.runtimeRootDirectory.path
        setenv("PSCALI_SYSFILES_ROOT", runtimeRoot, 1)
        let tmpPath = RuntimePaths.tmpDirectory.path
        setenv("TMPDIR", tmpPath, 1)
        setenv("SESSIONPATH", "\(tmpPath):~:.", 1)
        // Preferred docroot for sample web server inside sandbox.
        setenv("PSCALI_TEMP_DIR", RuntimePaths.sandboxVarHtdocsDirectory.path, 1)
        setenv("HOME", RuntimePaths.homeDirectory.path, 1)
        setenv("TERM", "xterm-256color", 1)
        setenv("COLORTERM", "truecolor", 1)
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

    private func writeWorkspaceBinVersionMarker() throws {
        try ensureWorkspaceDirectoriesExist()
        let data = (assetsVersion + "\n").data(using: .utf8) ?? Data()
        try data.write(to: RuntimePaths.workspaceBinVersionMarker, options: [.atomic])
    }

    private func writeWorkspaceSrcVersionMarker() throws {
        try ensureWorkspaceDirectoriesExist()
        let data = (assetsVersion + "\n").data(using: .utf8) ?? Data()
        try data.write(to: RuntimePaths.workspaceSrcVersionMarker, options: [.atomic])
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

    // Migrate old locations (home/bin, home/src/*) into workspace root.
    private func migrateLegacyBinIfNeeded() {
        let oldBin = RuntimePaths.homeDirectory.appendingPathComponent("bin", isDirectory: true)
        guard fileManager.fileExists(atPath: oldBin.path) else { return }
        let newBin = RuntimePaths.workspaceBinDirectory
        if fileManager.fileExists(atPath: newBin.path) { return }
        do {
            try fileManager.moveItem(at: oldBin, to: newBin)
        } catch {
            NSLog("PSCAL iOS: failed to migrate legacy bin: %@", error.localizedDescription)
        }
    }

    private func migrateLegacySrcIfNeeded() {
        let oldCompiler = RuntimePaths.homeDirectory.appendingPathComponent("src/compiler", isDirectory: true)
        let oldCore = RuntimePaths.homeDirectory.appendingPathComponent("src/core", isDirectory: true)
        let newCompiler = RuntimePaths.workspaceSrcCompilerDirectory
        let newCore = RuntimePaths.workspaceSrcCoreDirectory
        do {
            if fileManager.fileExists(atPath: oldCompiler.path) && !fileManager.fileExists(atPath: newCompiler.path) {
                try fileManager.createDirectory(at: newCompiler.deletingLastPathComponent(), withIntermediateDirectories: true)
                try fileManager.moveItem(at: oldCompiler, to: newCompiler)
            }
            if fileManager.fileExists(atPath: oldCore.path) && !fileManager.fileExists(atPath: newCore.path) {
                try fileManager.createDirectory(at: newCore.deletingLastPathComponent(), withIntermediateDirectories: true)
                try fileManager.moveItem(at: oldCore, to: newCore)
            }
        } catch {
            NSLog("PSCAL iOS: failed to migrate legacy src: %@", error.localizedDescription)
        }
    }

    // --- Embedded fallbacks to guarantee Tiny assets ship even if the bundle is missing bin/src ---

    // Keep in sync with repository bin/tiny (shortened copy to avoid missing asset errors)
    private let embeddedTinyWrapper: String = """
#!/bin/exsh
# Tiny wrapper for iOS/iPadOS: run the clike source via shell builtin.
if [ -n "$PSCALI_WORKSPACE_ROOT" ]; then
  SCRIPT="$PSCALI_WORKSPACE_ROOT/bin/tiny.clike"
else
  SCRIPT="$(dirname "$(dirname "$0")")/bin/tiny.clike"
fi
clike "$SCRIPT" "$@"
"""

    private let embeddedTinySource: String = """
#!/usr/bin/env clike
/* tiny compiler fallback */
str readFile(str path){mstream ms=mstreamcreate();if(!mstreamloadfromfile(&ms,path)){printf("Error: unable to read %s\\n",path);halt(1);}str buf=mstreambuffer(ms);mstreamfree(&ms);return buf;}
const int MAX_OPS=512;str opNames[MAX_OPS];int opCount=0;void loadOpcodes(str root){str header=root+"/src/compiler/bytecode.h";str buf=readFile(header);int inEnum=0;int i=1;int len=length(buf);while(i<=len){int j=i;while(j<=len&&copy(buf,j,1)!="\\n")j=j+1;int llen=j-i;str line=copy(buf,i,llen);i=j+1;int p=1;while(p<=llen&&copy(line,p,1)<=" ")p=p+1;str trimmed=copy(line,p,llen-p+1);if(pos("typedef enum",trimmed)==1){inEnum=1;continue;}if(!inEnum)continue;if(trimmed=="}"){break;}int comma=pos(",",trimmed);if(comma==0)continue;str name=copy(trimmed,1,comma-1);int k=length(name);while(k>0&&copy(name,k,1)<=" "){name=copy(name,1,k-1);k=k-1;}if(opCount<MAX_OPS){opNames[opCount]=name;opCount=opCount+1;}}}
int opcode(str name){for(int i=0;i<opCount;i=i+1)if(opNames[i]==name)return i;str pref="OP_"+name;for(int i=0;i<opCount;i=i+1)if(opNames[i]==pref)return i;printf("Unknown opcode %s\\n",name);halt(1);return -1;}
const int TYPE_INTEGER=2;const int TYPE_STRING=4;const int INLINE_CACHE_SLOT_SIZE=8;const int MAX_CONST=2048;int constType[MAX_CONST];str constVal[MAX_CONST];int constCount=0;
int addConstInt(int v){str s=inttostr(v);for(int i=0;i<constCount;i=i+1)if(constType[i]==TYPE_INTEGER&&constVal[i]==s)return i;int idx=constCount;constCount=constCount+1;constType[idx]=TYPE_INTEGER;constVal[idx]=s;return idx;}
int addConstStr(str s){for(int i=0;i<constCount;i=i+1)if(constType[i]==TYPE_STRING&&constVal[i]==s)return i;int idx=constCount;constCount=constCount+1;constType[idx]=TYPE_STRING;constVal[idx]=s;return idx;}
const int MAX_CODE=65536;int code[MAX_CODE];int codeLen=0;void emit(int b){code[codeLen]=b&255;codeLen=codeLen+1;}void emitShort(int v){emit((v>>8)&255);emit(v&255);}void emitICS(){for(int i=0;i<INLINE_CACHE_SLOT_SIZE;i=i+1)emit(0);}
const int MAX_TOK=4096;str tokType[MAX_TOK];str tokVal[MAX_TOK];int tokCount=0;int tokPos=0;
void tokenize(str src){tokCount=0;tokPos=0;while(1){int a=pos(src,"{");int b=pos(src,"}");if(a>0&&b>a)src=copy(src,1,a-1)+" "+copy(src,b+1,length(src)-b);else break;}int i=1;int n=length(src);while(i<=n){while(i<=n&&(copy(src,i,1)==" "||copy(src,i,1)=="\\n"||copy(src,i,1)=="\\t"||copy(src,i,1)=="\\r"))i=i+1;if(i>n)break;str c=copy(src,i,1);if(c=="{"){while(i<=n&&copy(src,i,1)!="}")i=i+1;i=i+1;continue;}if((c>="A"&&c<="Z")||(c>="a"&&c<="z")||c=="_"){int j=i;while(j<=n){str d=copy(src,j,1);if(!((d>="A"&&d<="Z")||(d>="a"&&d<="z")||(d>="0"&&d<="9")||d=="_"))break;j=j+1;}str id=copy(src,i,j-i);str low=id;if(low=="if"||low=="then"||low=="else"||low=="end"||low=="repeat"||low=="until"||low=="read"||low=="write")tokType[tokCount]=low;else tokType[tokCount]="IDENT";tokVal[tokCount]=id;tokCount=tokCount+1;i=j;continue;}if(c>="0"&&c<="9"){int j=i;while(j<=n&&copy(src,j,1)>="0"&&copy(src,j,1)<="9")j=j+1;tokType[tokCount]="NUMBER";tokVal[tokCount]=copy(src,i,j-i);tokCount=tokCount+1;i=j;continue;}if(c==":"&&i+1<=n&&copy(src,i+1,1)=="="){tokType[tokCount]=":=";tokVal[tokCount]=":=";tokCount=tokCount+1;i=i+2;continue;}if(c=="+"||c=="-"||c=="*"||c=="/"||c=="<"||c=="="||c==";"||c=="("||c==")"){tokType[tokCount]=c;tokVal[tokCount]=c;tokCount=tokCount+1;i=i+1;continue;}printf("Unknown token starting at %s\\n",copy(src,i,5));halt(1);}tokType[tokCount]="EOF";tokVal[tokCount]="";tokCount=tokCount+1;}
str curType(){return tokType[tokPos];}str curVal(){return tokVal[tokPos];}void consume(str t){if(curType()!=t){printf("Expected %s got %s\\n",t,curType());halt(1);}tokPos=tokPos+1;}
const int N_PROGRAM=1,N_ASSIGN=2,N_READ=3,N_WRITE=4,N_IF=5,N_REPEAT=6,N_BINOP=7,N_NUM=8,N_VAR=9;const int MAX_NODES=4096;int nType[MAX_NODES];int nA[MAX_NODES];str nS[MAX_NODES];int nChildren[MAX_NODES][8];int nChildCount[MAX_NODES];int nCount=0;
int newNode(int t){int idx=nCount;nType[idx]=t;nA[idx]=0;nS[idx]="";nChildCount[idx]=0;nCount=nCount+1;return idx;}void addChild(int p,int c){nChildren[p][nChildCount[p]]=c;nChildCount[p]=nChildCount[p]+1;}
int parseProgram();int parseStmtSeq();int parseStatement();int parseIf();int parseRepeat();int parseExpr();int parseSimpleExpr();int parseTerm();int parseFactor();
int parseProgram(){int p=newNode(N_PROGRAM);int seq=parseStmtSeq();addChild(p,seq);consume("EOF");return p;}
int parseStmtSeq(){int seq=newNode(N_PROGRAM);addChild(seq,parseStatement());while(curType()==";"){consume(";");addChild(seq,parseStatement());}return seq;}
int parseStatement(){str t=curType();if(t=="if")return parseIf();if(t=="repeat")return parseRepeat();if(t=="read"){consume("read");int v=newNode(N_READ);nS[v]=curVal();consume("IDENT");return v;}if(t=="write"){consume("write");int w=newNode(N_WRITE);if(curType()!=";"&&curType()!="until"&&curType()!="end"&&curType()!="else"&&curType()!="EOF")addChild(w,parseExpr());return w;}if(t=="IDENT"){str name=curVal();consume("IDENT");consume(":=");int a=newNode(N_ASSIGN);nS[a]=name;addChild(a,parseExpr());return a;}printf("Unexpected token %s\\n",t);halt(1);return -1;}
int parseIf(){consume("if");int n=newNode(N_IF);int cond=parseExpr();addChild(n,cond);consume("then");addChild(n,parseStmtSeq());if(curType()=="else"){consume("else");addChild(n,parseStmtSeq());}consume("end");return n;}
int parseRepeat(){consume("repeat");int n=newNode(N_REPEAT);addChild(n,parseStmtSeq());consume("until");addChild(n,parseExpr());return n;}
int parseExpr(){int n=parseSimpleExpr();if(curType()=="<"||curType()=="="){str op=curType();consume(op);int r=parseSimpleExpr();int b=newNode(N_BINOP);nS[b]=op;addChild(b,n);addChild(b,r);return b;}return n;}
int parseSimpleExpr(){int n=parseTerm();while(curType()=="+"||curType()=="-"){str op=curType();consume(op);int r=parseTerm();int b=newNode(N_BINOP);nS[b]=op;addChild(b,n);addChild(b,r);n=b;}return n;}
int parseTerm(){int n=parseFactor();while(curType()=="*"||curType()=="/"){str op=curType();consume(op);int r=parseFactor();int b=newNode(N_BINOP);nS[b]=op;addChild(b,n);addChild(b,r);n=b;}return n;}
int parseFactor(){str t=curType();if(t=="("){consume("(");int n=parseExpr();consume(")");return n;}if(t=="NUMBER"){str v=curVal();consume("NUMBER");int n=newNode(N_NUM);nA[n]=atoi(v);return n;}if(t=="IDENT"){str v=curVal();consume("IDENT");int n=newNode(N_VAR);nS[n]=v;return n;}printf("Unexpected token %s\\n",t);halt(1);return -1;}
str vars[MAX_TOK];int varCount=0;int varIdx(str name){for(int i=0;i<varCount;i=i+1)if(vars[i]==name)return i;return -1;}void collectVars(int node){int t=nType[node];if(t==N_VAR||t==N_READ||t==N_ASSIGN){str nm=nS[node];if(varIdx(nm)<0){vars[varCount]=nm;varCount=varCount+1;}}for(int i=0;i<nChildCount[node];i=i+1)collectVars(nChildren[node][i]);}
int nameConstIdx[MAX_TOK];int readConst,writeConst,trueConst,falseConst,typeIntConst;
void emitConst(int idx){if(idx<=255){emit(opcode("CONSTANT"));emit(idx);}else{emit(opcode("CONSTANT16"));emitShort(idx);} }
void emitDefine(str name){int idx=nameConstIdx[varIdx(name)];if(idx<=255){emit(opcode("DEFINE_GLOBAL"));emit(idx);}else{emit(opcode("DEFINE_GLOBAL16"));emitShort(idx);}emit(TYPE_INTEGER);emitShort(typeIntConst);}
void emitGetGlobal(str name){int idx=nameConstIdx[varIdx(name)];if(idx<=255){emit(opcode("GET_GLOBAL"));emit(idx);}else{emit(opcode("GET_GLOBAL16"));emitShort(idx);}emitICS();}
void emitGetGlobalAddr(str name){int idx=nameConstIdx[varIdx(name)];if(idx<=255){emit(opcode("GET_GLOBAL_ADDRESS"));emit(idx);}else{emit(opcode("GET_GLOBAL_ADDRESS16"));emitShort(idx);} }
void compileExpr(int node){int t=nType[node];if(t==N_NUM){emitConst(addConstInt(nA[node]));return;}if(t==N_VAR){emitGetGlobal(nS[node]);return;}if(t==N_BINOP){compileExpr(nChildren[node][0]);compileExpr(nChildren[node][1]);str op=nS[node];if(op=="+")emit(opcode("ADD"));else if(op=="-")emit(opcode("SUBTRACT"));else if(op=="*")emit(opcode("MULTIPLY"));else if(op=="/")emit(opcode("DIVIDE"));else if(op=="<")emit(opcode("LESS"));else if(op=="=")emit(opcode("EQUAL"));else{printf("Unknown op %s\\n",op);halt(1);}return;}printf("Bad expr node %d\\n",t);halt(1);}
void compileStmt(int node){int t=nType[node];if(t==N_ASSIGN){compileExpr(nChildren[node][0]);emitGetGlobalAddr(nS[node]);emit(opcode("SWAP"));emit(opcode("SET_INDIRECT"));return;}if(t==N_READ){emitGetGlobalAddr(nS[node]);emit(opcode("CALL_BUILTIN"));emitShort(readConst);emit(1);return;}if(t==N_WRITE){if(nChildCount[node]==0){emitConst(trueConst);emit(opcode("CALL_BUILTIN"));emitShort(writeConst);emit(1);}else{emitConst(falseConst);compileExpr(nChildren[node][0]);emit(opcode("CALL_BUILTIN"));emitShort(writeConst);emit(2);}return;}if(t==N_IF){compileExpr(nChildren[node][0]);emit(opcode("JUMP_IF_FALSE"));int jElse=codeLen;emitShort(0);int thenNode=nChildren[node][1];for(int i=0;i<nChildCount[thenNode];i=i+1)compileStmt(nChildren[thenNode][i]);if(nChildCount[node]>2){emit(opcode("JUMP"));int jEnd=codeLen;emitShort(0);int off=codeLen-(jElse+2);code[jElse]=(off>>8)&255;code[jElse+1]=off&255;int elseNode=nChildren[node][2];for(int i=0;i<nChildCount[elseNode];i=i+1)compileStmt(nChildren[elseNode][i]);off=codeLen-(jEnd+2);code[jEnd]=(off>>8)&255;code[jEnd+1]=off&255;}else{int off=codeLen-(jElse+2);code[jElse]=(off>>8)&255;code[jElse+1]=off&255;}return;}if(t==N_REPEAT){int loopStart=codeLen;int body=nChildren[node][0];for(int i=0;i<nChildCount[body];i=i+1)compileStmt(nChildren[body][i]);compileExpr(nChildren[node][1]);emit(opcode("JUMP_IF_FALSE"));int back=loopStart-(codeLen+2);emitShort(back);return;}printf("Bad stmt node %d\\n",t);halt(1);}
void compileProgram(int rootNode){for(int i=0;i<varCount;i=i+1)emitDefine(vars[i]);int seq=nChildren[rootNode][0];for(int i=0;i<nChildCount[seq];i=i+1)compileStmt(nChildren[seq][i]);emitConst(trueConst);emit(opcode("CALL_BUILTIN"));emitShort(writeConst);emit(1);emit(opcode("HALT"));}
int vmVersion(str root){str buf=readFile(root+"/src/core/version.h");int i=1;int n=length(buf);while(i<=n){int j=i;while(j<=n&&copy(buf,j,1)!="\\n")j=j+1;str line=copy(buf,i,j-i);if(pos("PSCAL_VM_VERSION",line)>0){int k=pos("PSCAL_VM_VERSION",line)+length("PSCAL_VM_VERSION");while(k<=length(line)&&copy(line,k,1)==" ")k=k+1;int start=k;while(k<=length(line)&&copy(line,k,1)>="0"&&copy(line,k,1)<="9")k=k+1;return atoi(copy(line,start,k-start));}i=j+1;}printf("VM version not found\\n");halt(1);return 0;}
str appendByte(str s,int b){return s+tochar(b&255);}str append32(str s,int v){s=appendByte(s,v);s=appendByte(s,(v>>8));s=appendByte(s,(v>>16));s=appendByte(s,(v>>24));return s;}str append64(str s,long long v){int lo=(int)(v&4294967295);int hi=(int)((v>>32)&4294967295);s=append32(s,lo);s=append32(s,hi);return s;}
void writePbc(str outPath,int version){str out=\"\";int magic=1347634098;out=append32(out,magic);out=append32(out,version);out=append64(out,0);out=append64(out,0);str pathLit=\"tiny\";out=append32(out,-length(pathLit));out=out+pathLit;out=append32(out,codeLen);out=append32(out,constCount);for(int i=0;i<codeLen;i=i+1)out=appendByte(out,code[i]);for(int i=0;i<codeLen;i=i+1)out=append32(out,0);for(int i=0;i<constCount;i=i+1){out=append32(out,constType[i]);if(constType[i]==TYPE_INTEGER){long long v=atoi(constVal[i]);out=append64(out,v);}else{str s=constVal[i];if(s==\"\"){out=append32(out,-1);}else{out=append32(out,length(s));out=out+s;}}}mstream ms=mstreamfromstring(out);mstreamsavetofile(&ms,outPath);mstreamfree(&ms);}
int main(){if(paramcount()<2){printf("Usage: clike bin/tiny <source.tiny> <out.pbc>\\n");return 1;}str srcPath=paramstr(1);str outPath=paramstr(2);str root=dirname(dirname(paramstr(0)));if(root==\"/\"||root==\"\"){root=getenv(\"PSCALI_WORKSPACE_ROOT\");if(root==\"\")root=getenv(\"PSCAL_WORKSPACE_ROOT\");if(root==\"\")root=\".\";}loadOpcodes(root);int version=vmVersion(root);str source=readFile(srcPath);nCount=0;varCount=0;constCount=0;codeLen=0;tokenize(source);int ast=parseProgram();collectVars(ast);readConst=addConstStr(\"read\");writeConst=addConstStr(\"write\");trueConst=addConstInt(1);falseConst=addConstInt(0);typeIntConst=addConstStr(\"integer\");for(int i=0;i<varCount;i=i+1)nameConstIdx[i]=addConstStr(vars[i]);compileProgram(ast);writePbc(outPath,version);return 0;}
"""

    // Minimal header snippets Tiny reads; enough to parse VM_VERSION/opcodes
    private let embeddedBytecodeHeader: String = """
// tiny fallback header
typedef enum { RETURN, CONSTANT, CONSTANT16, CONST_0, CONST_1, CONST_TRUE, CONST_FALSE, PUSH_IMMEDIATE_INT8, ADD, SUBTRACT, MULTIPLY, DIVIDE, NEGATE, NOT, TO_BOOL, EQUAL, NOT_EQUAL, GREATER, GREATER_EQUAL, LESS, LESS_EQUAL, INT_DIV, MOD, AND, OR, XOR, SHL, SHR, JUMP_IF_FALSE, JUMP, SWAP, DUP, DEFINE_GLOBAL, DEFINE_GLOBAL16, GET_GLOBAL, GET_GLOBAL16, GET_GLOBAL_ADDRESS, GET_GLOBAL_ADDRESS16, CONSTANT_STRING, CONSTANT_STRING16, CALL_BUILTIN } Opcode;
#define GLOBAL_INLINE_CACHE_SLOT_SIZE 8
"""

    private let embeddedVersionHeader: String = """
#ifndef PSCAL_VERSION_H
#define PSCAL_VERSION_H
#define PSCAL_VM_VERSION 9
#endif
"""
}
