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

    static var workspaceFontsDirectory: URL {
        documentsDirectory.appendingPathComponent("fonts", isDirectory: true)
    }

    static var workspaceLibSoundsDirectory: URL {
        documentsDirectory.appendingPathComponent("lib/sounds", isDirectory: true)
    }

    static var legacySysfilesDirectory: URL {
        documentsDirectory.appendingPathComponent("sysfiles", isDirectory: true)
    }

    static var sandboxVarHtdocsDirectory: URL {
        documentsDirectory.appendingPathComponent("var/htdocs", isDirectory: true)
    }

    static var sandboxVarLogDirectory: URL {
        documentsDirectory.appendingPathComponent("var/log", isDirectory: true)
    }
}

final class RuntimeAssetInstaller {
    static let shared = RuntimeAssetInstaller()

    private let fileManager = FileManager.default
    private let workspaceInstallLock = NSLock()
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
        let initialDstPtr = UnsafeMutablePointer<UInt8>.allocate(capacity: 1)
        let initialSrcMutable = UnsafeMutablePointer<UInt8>.allocate(capacity: 1)
        defer {
            initialDstPtr.deallocate()
            initialSrcMutable.deallocate()
        }

        var stream = compression_stream(dst_ptr: initialDstPtr,
                                        dst_size: 0,
                                        src_ptr: UnsafePointer<UInt8>(initialSrcMutable),
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
        workspaceInstallLock.lock()
        defer { workspaceInstallLock.unlock() }

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
        installWorkspaceFontsIfNeeded(bundleRoot: bundleRoot)
        installWorkspaceSoundAssetsIfNeeded(bundleRoot: bundleRoot)
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

        let deflated = Bundle.main.url(forResource: "pscal_tool_runner", withExtension: "deflate")
        let bundledRunner = Bundle.main.url(forResource: "pscal_tool_runner", withExtension: nil)
        let stagedRunner = RuntimePaths.stagedToolRunner

        let stageRunner: (Data) -> String? = { payload in
            do {
                try self.ensureDocumentsDirectoryExists()
                if self.fileManager.fileExists(atPath: stagedRunner.path) {
                    try self.fileManager.removeItem(at: stagedRunner)
                }
                try payload.write(to: stagedRunner, options: .atomic)
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
            if let decompressed = self.decompressDeflate(data),
               let path = stageRunner(decompressed) {
                return path
            }
            NSLog("PSCAL iOS: failed to decompress tool runner payload.")
        }

        // Fallback: if an old-style raw runner is present in the bundle, stage it.
        if let bundledRunner,
           fileManager.isExecutableFile(atPath: bundledRunner.path),
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
            do {
                let copiedCount = try copyMissingItemsWithCount(from: bundledExamples, to: workspaceExamples)
                if copiedCount > 0 {
                    NSLog("PSCAL iOS: ensured Examples workspace at %@ (installed %d missing file(s))",
                          workspaceExamples.path, copiedCount)
                }
            } catch {
                NSLog("PSCAL iOS: failed to sync missing Examples files at %@: %@",
                      workspaceExamples.path, error.localizedDescription)
            }
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
            let fallbackNames = ["pscal_LICENSE.txt", "openssl_LICENSE.txt", "curl_LICENSE.txt", "sdl_LICENSE.txt", "nextvi_LICENSE.txt", "micro_LICENSE.txt", "openssh_LICENSE.txt", "libgit2_LICENSE.txt", "yyjson_LICENSE.txt", "hterm_LICENSE.txt"]
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
        ensureEtcFileNamed("hosts", bundleRoot: bundleRoot)
        ensureEtcFileNamed("fstab", bundleRoot: bundleRoot)
        // Ensure word lists are always present even if workspace/etc already exists.
        ensureEtcFileNamed("words", bundleRoot: bundleRoot)
        ensureEtcFileNamed("words.short", bundleRoot: bundleRoot)
        ensureEtcFileNamed("words.many", bundleRoot: bundleRoot)
        ensureEtcFileNamed("rc",
                           bundleRoot: bundleRoot,
                           executable: true,
                           replaceExisting: true,
                           replaceExistingIfDifferent: true)
        installWorkspaceRcScript()
        ensureEtcSubdirectoryNamed("ssh", bundleRoot: bundleRoot)
        ensureWorkspaceEtcSubdirectoryNamed("service")
    }

    private func installWorkspaceBinIfNeeded(bundleRoot: URL) {
        let bundledBin = bundleRoot.appendingPathComponent("bin", isDirectory: true)
        let workspaceBin = RuntimePaths.workspaceBinDirectory
        do {
            let refreshNeeded = needsWorkspaceBinRefresh()
            try ensureDocumentsDirectoryExists()
            migrateLegacyBinIfNeeded()
            try fileManager.createDirectory(at: workspaceBin, withIntermediateDirectories: true)

            var copiedCount = 0
            var bundledIsDirectory: ObjCBool = false
            if fileManager.fileExists(atPath: bundledBin.path, isDirectory: &bundledIsDirectory),
               bundledIsDirectory.boolValue {
                // The Tiny compiler belongs to the app, not the user, so keep
                // it identical to the bundle: an update must replace a stale
                // copy, not just fill in a missing one. bin/tiny runs tiny.pbc
                // in preference to tiny.clike, so a tiny.pbc the bundle does
                // not ship is left over from an older release and has to go.
                for name in ["tiny", "tiny.clike", "tiny.pbc"] {
                    let bundled = bundledBin.appendingPathComponent(name, isDirectory: false)
                    let installed = workspaceBin.appendingPathComponent(name, isDirectory: false)
                    if fileManager.fileExists(atPath: bundled.path) {
                        if try replaceIfChanged(installed, with: bundled) {
                            copiedCount += 1
                        }
                    } else if fileManager.fileExists(atPath: installed.path) || isSymbolicLink(at: installed) {
                        try fileManager.removeItem(at: installed)
                        NSLog("PSCAL iOS: removed stale %@", installed.path)
                    }
                }
                copiedCount += try copyMissingItemsWithCount(from: bundledBin, to: workspaceBin)
            } else {
                NSLog("PSCAL iOS: bundle missing bin directory; preserving existing workspace bin at %@", workspaceBin.path)
            }

            let tinyWrapper = workspaceBin.appendingPathComponent("tiny", isDirectory: false)
            if fileManager.fileExists(atPath: tinyWrapper.path) {
                try markExecutable(at: tinyWrapper)
            } else {
                NSLog("PSCAL iOS: tiny wrapper not found after install at %@", tinyWrapper.path)
            }
            try writeWorkspaceBinVersionMarker()
            if copiedCount > 0 || refreshNeeded {
                NSLog("PSCAL iOS: ensured bin assets at %@ (wrote %d file(s))",
                      workspaceBin.path,
                      copiedCount)
            }
        } catch {
            NSLog("PSCAL iOS: failed to install bin assets: %@", error.localizedDescription)
        }
    }

    // tiny.clike reads opcodes.def, version.h and var_type.h from
    // $PSCALI_WORKSPACE_ROOT/src when it compiles, and they must describe the
    // VM this build runs. Mirror both directories from the bundle on every
    // launch rather than only when the build number changes: a development
    // build that keeps its number can still change the VM.
    private func installWorkspaceSrcIfNeeded(bundleRoot: URL) {
        migrateLegacySrcIfNeeded()
        let directories = [
            (bundleRoot.appendingPathComponent("src/compiler", isDirectory: true),
             RuntimePaths.workspaceSrcCompilerDirectory),
            (bundleRoot.appendingPathComponent("src/core", isDirectory: true),
             RuntimePaths.workspaceSrcCoreDirectory)
        ]
        for (bundled, workspace) in directories {
            var isDirectory: ObjCBool = false
            guard fileManager.fileExists(atPath: bundled.path, isDirectory: &isDirectory),
                  isDirectory.boolValue else {
                NSLog("PSCAL iOS: bundle missing %@; tiny cannot compile", bundled.path)
                continue
            }
            do {
                let copiedCount = try mirrorFiles(from: bundled, to: workspace)
                if copiedCount > 0 {
                    NSLog("PSCAL iOS: installed %d tiny header file(s) into %@", copiedCount, workspace.path)
                }
            } catch {
                NSLog("PSCAL iOS: failed to install tiny headers into %@: %@",
                      workspace.path, error.localizedDescription)
            }
        }
    }

    private func installWorkspaceFontsIfNeeded(bundleRoot: URL) {
        let bundledFonts = bundleRoot.appendingPathComponent("fonts", isDirectory: true)
        guard fileManager.fileExists(atPath: bundledFonts.path) else { return }
        let workspaceFonts = RuntimePaths.workspaceFontsDirectory
        do {
            try fileManager.createDirectory(at: workspaceFonts, withIntermediateDirectories: true)
            let copiedCount = try copyMissingItemsWithCount(from: bundledFonts, to: workspaceFonts)
            if copiedCount > 0 {
                NSLog("PSCAL iOS: ensured fonts assets at %@ (installed %d missing file(s))",
                      workspaceFonts.path, copiedCount)
            }
        } catch {
            NSLog("PSCAL iOS: failed to stage fonts assets into workspace (%@): %@",
                  workspaceFonts.path, error.localizedDescription)
        }
    }

    private func installWorkspaceSoundAssetsIfNeeded(bundleRoot: URL) {
        let bundledSounds = bundleRoot.appendingPathComponent("lib/sounds", isDirectory: true)
        guard fileManager.fileExists(atPath: bundledSounds.path) else {
            NSLog("PSCAL iOS: bundle missing lib/sounds directory; skipping sound staging.")
            return
        }

        let targets: [URL] = [
            RuntimePaths.workspaceLibSoundsDirectory,
            RuntimePaths.homeDirectory.appendingPathComponent("lib/sounds", isDirectory: true)
        ]

        for target in targets {
            do {
                try fileManager.createDirectory(at: target, withIntermediateDirectories: true)
                let copiedCount = try copyMissingItemsWithCount(from: bundledSounds, to: target)
                if copiedCount > 0 {
                    NSLog("PSCAL iOS: ensured sound assets at %@ (installed %d missing file(s))",
                          target.path, copiedCount)
                }
            } catch {
                NSLog("PSCAL iOS: failed to stage sound assets into %@: %@",
                      target.path, error.localizedDescription)
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
        let tinySource = binDir.appendingPathComponent("tiny.clike")
        guard fileManager.fileExists(atPath: tinySource.path) else {
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

    private func ensureEtcFileNamed(_ name: String,
                                    bundleRoot: URL,
                                    executable: Bool = false,
                                    replaceExisting: Bool = false,
                                    replaceExistingIfDifferent: Bool = false) {
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
            } else if replaceExisting {
                try fileManager.removeItem(at: workspaceFile)
                try fileManager.copyItem(at: bundleFile, to: workspaceFile)
            } else if replaceExistingIfDifferent {
                let bundledData = try? Data(contentsOf: bundleFile)
                let workspaceData = try? Data(contentsOf: workspaceFile)
                let shouldReplace = (bundledData == nil || workspaceData == nil)
                    ? true
                    : (bundledData != workspaceData)
                if shouldReplace {
                    try fileManager.removeItem(at: workspaceFile)
                    try fileManager.copyItem(at: bundleFile, to: workspaceFile)
                }
            }
            if executable {
                try markExecutable(at: workspaceFile)
            }
        } catch {
            NSLog("PSCAL iOS: failed to ensure etc/%@: %@", name, error.localizedDescription)
        }
    }

    private func ensureWorkspaceEtcSubdirectoryNamed(_ name: String) {
        let workspaceSubdirectory = RuntimePaths.workspaceEtcDirectory.appendingPathComponent(name, isDirectory: true)
        do {
            try ensureWorkspaceDirectoriesExist()
            var isDirectory: ObjCBool = false
            let exists = fileManager.fileExists(atPath: workspaceSubdirectory.path, isDirectory: &isDirectory)
            if !exists {
                try fileManager.createDirectory(at: workspaceSubdirectory, withIntermediateDirectories: true)
            } else if !isDirectory.boolValue {
                try fileManager.removeItem(at: workspaceSubdirectory)
                try fileManager.createDirectory(at: workspaceSubdirectory, withIntermediateDirectories: true)
            }
        } catch {
            NSLog("PSCAL iOS: failed to ensure etc/%@ directory: %@", name, error.localizedDescription)
        }
    }

    private func installWorkspaceRcScript() {
        guard let scriptData = embeddedRcScript.data(using: .utf8) else {
            NSLog("PSCAL iOS: failed to encode embedded rc script")
            return
        }
        let workspaceRc = RuntimePaths.workspaceEtcDirectory.appendingPathComponent("rc", isDirectory: false)
        do {
            try ensureWorkspaceDirectoriesExist()
            ensureWorkspaceEtcSubdirectoryNamed("service")
            let existing = try? Data(contentsOf: workspaceRc)
            if existing != scriptData {
                try scriptData.write(to: workspaceRc, options: [.atomic])
            }
            try markExecutable(at: workspaceRc)
        } catch {
            NSLog("PSCAL iOS: failed to install embedded etc/rc: %@", error.localizedDescription)
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

    private func copyMissingItemsWithCount(from source: URL, to destination: URL) throws -> Int {
        var copiedCount = 0
        let entries = try fileManager.contentsOfDirectory(atPath: source.path)
        for entry in entries where entry != ".DS_Store" {
            let sourceURL = source.appendingPathComponent(entry)
            let destinationURL = destination.appendingPathComponent(entry)
            var isDirectory: ObjCBool = false
            guard fileManager.fileExists(atPath: sourceURL.path, isDirectory: &isDirectory) else {
                continue
            }
            if isDirectory.boolValue {
                var destinationIsDirectory: ObjCBool = false
                let destinationExists = fileManager.fileExists(atPath: destinationURL.path, isDirectory: &destinationIsDirectory)
                if !destinationExists {
                    try fileManager.createDirectory(at: destinationURL, withIntermediateDirectories: true)
                } else if !destinationIsDirectory.boolValue {
                    // Preserve existing non-directory entries.
                    continue
                }
                copiedCount += try copyMissingItemsWithCount(from: sourceURL, to: destinationURL)
            } else if !fileManager.fileExists(atPath: destinationURL.path) {
                try fileManager.copyItem(at: sourceURL, to: destinationURL)
                copiedCount += 1
            }
        }
        return copiedCount
    }

    /// Writes `source`'s contents to `destination` unless it already holds
    /// them. Returns whether it wrote.
    private func replaceIfChanged(_ destination: URL, with source: URL) throws -> Bool {
        let wanted = try Data(contentsOf: source)
        if let current = try? Data(contentsOf: destination), current == wanted {
            return false
        }
        try wanted.write(to: destination, options: [.atomic])
        return true
    }

    /// Makes `destination` a directory holding exactly the files in the flat
    /// directory `source`: writes any that are missing or differ and removes
    /// any that `source` lacks. Returns the number of files written.
    private func mirrorFiles(from source: URL, to destination: URL) throws -> Int {
        var isDirectory: ObjCBool = false
        if isSymbolicLink(at: destination) ||
            (fileManager.fileExists(atPath: destination.path, isDirectory: &isDirectory) && !isDirectory.boolValue) {
            try fileManager.removeItem(at: destination)
        }
        try fileManager.createDirectory(at: destination, withIntermediateDirectories: true)

        let entries = Set(try fileManager.contentsOfDirectory(atPath: source.path)).subtracting([".DS_Store"])
        for entry in try fileManager.contentsOfDirectory(atPath: destination.path) where !entries.contains(entry) {
            try fileManager.removeItem(at: destination.appendingPathComponent(entry))
        }
        var copiedCount = 0
        for entry in entries {
            if try replaceIfChanged(destination.appendingPathComponent(entry, isDirectory: false),
                                    with: source.appendingPathComponent(entry, isDirectory: false)) {
                copiedCount += 1
            }
        }
        return copiedCount
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
        let workspaceDefaultFontPath = "/fonts/Roboto/static/Roboto-Regular.ttf"
        let bundledDefaultFontPath = bundleRoot
            .appendingPathComponent("fonts/Roboto/static/Roboto-Regular.ttf")
            .path
        setenv("PSCAL_FONT_PATH", workspaceDefaultFontPath, 1)
        setenv("PSCAL_DEFAULT_FONT", bundledDefaultFontPath, 1)
        let workspaceSoundPath = "/lib/sounds:/home/lib/sounds"
        let bundledSoundPath = bundleRoot.appendingPathComponent("lib/sounds").path
        setenv("PSCAL_SOUND_PATH", "\(workspaceSoundPath):\(bundledSoundPath)", 1)
        // Preferred docroot for sample web server inside sandbox.
        setenv("PSCALI_TEMP_DIR", RuntimePaths.sandboxVarHtdocsDirectory.path, 1)
        setenv("HOME", RuntimePaths.homeDirectory.path, 1)
        setenv("TERM", "xterm-256color", 1)
        setenv("COLORTERM", "truecolor", 1)
        let processInfo = ProcessInfo.processInfo
        setenv("PSCAL_CPU_COUNT", String(processInfo.processorCount), 1)
        setenv("PSCAL_ACTIVE_CPU_COUNT", String(processInfo.activeProcessorCount), 1)
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
        let desiredContents = isExshrc ? migratedExshrcContents(newContents) : newContents
        if !fileManager.fileExists(atPath: destination.path) {
            do {
                try desiredContents.write(to: destination, atomically: true, encoding: .utf8)
            } catch {
                NSLog("PSCAL iOS: failed to write %@: %@", destination.lastPathComponent, error.localizedDescription)
            }
            return
        }
        // Preserve user-managed ~/.exshrc across launches. If users remove it entirely,
        // the missing-file path above will seed a fresh copy from etc/skel.
        if isExshrc {
            guard let oldData = try? Data(contentsOf: destination),
                  let oldContents = String(data: oldData, encoding: .utf8) else {
                return
            }
            let migratedContents = migratedExshrcContents(oldContents)
            guard migratedContents != oldContents else {
                return
            }
            do {
                try migratedContents.write(to: destination, atomically: true, encoding: .utf8)
            } catch {
                NSLog("PSCAL iOS: failed to migrate %@: %@", destination.lastPathComponent, error.localizedDescription)
            }
            return
        }
        // For other skel entries, only overwrite when contents differ.
        guard let oldData = try? Data(contentsOf: destination),
              let oldContents = String(data: oldData, encoding: .utf8),
              oldContents != desiredContents else {
            return
        }
        do {
            try desiredContents.write(to: destination, atomically: true, encoding: .utf8)
        } catch {
            NSLog("PSCAL iOS: failed to update %@: %@", destination.lastPathComponent, error.localizedDescription)
        }
    }

    private func migratedExshrcContents(_ contents: String) -> String {
        let pattern = #"(?m)^[ \t]*if \[ "\$\{PSCALSHELL_THREAD_METRICS:-0\}" != "0" \]; then\n[ \t]*echo "\[exsh\] worker pool snapshot"\n[ \t]*threadpool_report\n[ \t]*fi\n?"#
        guard let regex = try? NSRegularExpression(pattern: pattern) else {
            return contents
        }
        let range = NSRange(contents.startIndex..<contents.endIndex, in: contents)
        let updated = regex.stringByReplacingMatches(in: contents,
                                                     options: [],
                                                     range: range,
                                                     withTemplate: "")
        guard updated != contents else {
            return contents
        }
        var normalized = updated
        while normalized.contains("\n\n\n") {
            normalized = normalized.replacingOccurrences(of: "\n\n\n", with: "\n\n")
        }
        return normalized
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
            RuntimePaths.tmpDirectory,
            RuntimePaths.sandboxVarLogDirectory
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

    // Keep in sync with the repository's etc/rc; installWorkspaceRcScript writes it over the workspace copy.
    private let embeddedRcScript: String = """
#!/bin/exsh

# Basic init bootstrap for iOS/iPadOS service-mode validation.
etc_root="${PSCALI_ETC_ROOT:-/etc}"
service_dir_real="$etc_root/service"
service_dir="/etc/service"

log_dir="/var/log"
if ! mkdir -p "$log_dir" 2>/dev/null; then
    sandbox_root=""
    if [ -n "$etc_root" ]; then
        sandbox_root="$(dirname "$etc_root")"
    fi
    if [ -z "$sandbox_root" ] && [ -n "$PATH_TRUNCATE" ]; then
        sandbox_root="$PATH_TRUNCATE"
    fi
    if [ -z "$sandbox_root" ] && [ -n "$PSCALI_CONTAINER_ROOT" ]; then
        sandbox_root="$PSCALI_CONTAINER_ROOT/Documents"
    fi
    if [ -n "$sandbox_root" ]; then
        log_dir="$sandbox_root/var/log"
        mkdir -p "$log_dir"
    fi
fi

rm -f "$log_dir"/rc-nodate-*.log 2>/dev/null

stamp="pid$$"
log="$log_dir/rc-${stamp}.log"
cwd="$(pwd 2>/dev/null)"
if [ -z "$cwd" ]; then
    cwd="(unknown)"
fi
echo "smallclue /etc/rc start" > "$log" 2>&1
echo "pid=$$" >> "$log" 2>&1
echo "stamp=$stamp" >> "$log" 2>&1
echo "date=$(date 2>/dev/null)" >> "$log" 2>&1
echo "cwd=$cwd" >> "$log" 2>&1
echo "etc_root=$etc_root" >> "$log" 2>&1
echo "service_dir=$service_dir" >> "$log" 2>&1
echo "service_dir_real=$service_dir_real" >> "$log" 2>&1
echo "log_dir=$log_dir" >> "$log" 2>&1
echo "rc_format=v2" >> "$log" 2>&1

# Keep a lightweight heartbeat so we can verify init+rc lifecycle over time.
(
    while true; do
        sleep 600
        echo "heartbeat date=$(date 2>/dev/null)" >> "$log" 2>&1
    done
) &
echo "heartbeat logger pid=$!" >> "$log" 2>&1

if mkdir -p "$service_dir_real" 2>/dev/null; then
    runit "$service_dir" >> "$log" 2>&1 &
    echo "runit started pid=$!" >> "$log" 2>&1
else
    echo "failed to create service directory; skipping runit startup" >> "$log" 2>&1
fi

echo "smallclue /etc/rc done" >> "$log" 2>&1
exit 0
"""
}
