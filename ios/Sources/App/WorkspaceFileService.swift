// Copyright 2026 Mike Miller
// SPDX-License-Identifier: Apache-2.0
//
// Ported by its author from iSH-AOK's Workspace (app/GuestFileBridge.h at 0f75fee1c) and
// licensed to PSCAL under the Apache License 2.0.

import Foundation

@_silgen_name("pathTruncateStrip")
private func pathTruncateStripC(_ absolutePath: UnsafePointer<CChar>, _ out: UnsafeMutablePointer<CChar>, _ size: Int) -> Bool

@_silgen_name("pscalPathVirtualized_hostPath")
private func pscalPathVirtualizedHostPathC(_ path: UnsafePointer<CChar>, _ expanded: UnsafeMutablePointer<CChar>) -> UnsafePointer<CChar>?

enum WorkspaceFileKind {
    case regular, directory, symlink, fifo, socket, charDevice, blockDevice, other
}

/// One filesystem entry. For a symlink, `kind` is the target's, so a symlinked folder shows
/// as a folder, unless the link is broken, when `kind` is `.symlink`.
struct WorkspaceFileItem {
    let name: String
    /// As the shell sees it.
    let path: String
    let kind: WorkspaceFileKind
    let size: UInt64
    let modified: Date?
    let posixMode: mode_t
    let symlinkTarget: String?

    var isSymlink: Bool { symlinkTarget != nil }
    var isBrokenSymlink: Bool { kind == .symlink }
}

enum WorkspaceFileError: LocalizedError {
    case cancelled
    case emptyPath
    case tooLarge
    case notRegularFile
    case refusedRoot
    case refusedMount
    case system(Int32)

    var errorDescription: String? {
        switch self {
        case .cancelled: return "Cancelled."
        case .emptyPath: return "No path was given."
        case .tooLarge: return "The file is too large to open here."
        case .notRegularFile: return "That isn't a regular file."
        case .refusedRoot: return "This folder holds the app's own files, so it can't be removed."
        case .refusedMount: return "This is a mounted folder. Use umount to take it away from here."
        case .system(let code): return String(cString: strerror(code))
        }
    }
}

/// Lets a caller that navigated away stop the work rather than only ignore the answer. A
/// delete that has begun finishes: stopping a tree walk halfway leaves no way to say what
/// survived.
final class WorkspaceFileOperation: @unchecked Sendable {
    private let lock = NSLock()
    private var cancelledValue = false

    var isCancelled: Bool {
        lock.lock()
        defer { lock.unlock() }
        return cancelledValue
    }

    func cancel() {
        lock.lock()
        cancelledValue = true
        lock.unlock()
    }
}

/// The app's files for the applets. Paths are as the shell sees them, so a path typed in a
/// terminal and one shown in the File Manager are the same text; this turns them into real
/// ones. Every operation runs off the main thread and completes on it.
///
/// Two serial queues, because one meant a large copy sat in front of a twelve-entry listing
/// and froze the file browser. Work on the same path keeps its order by chaining: the
/// completion runs after the effects are visible, so write-then-reload goes in the write's
/// completion.
final class WorkspaceFileService: @unchecked Sendable {
    static let shared = WorkspaceFileService()
    /// Up to here is the interactive lane.
    private static let interactiveLimit = 8 * 1024 * 1024

    private let interactive = DispatchQueue(label: "com.pscal.workspace.files.interactive", qos: .userInitiated)
    private let bulk = DispatchQueue(label: "com.pscal.workspace.files.bulk", qos: .utility)

    // MARK: - Paths

    /// The real path behind one the shell sees.
    static func hostPath(_ shown: String) -> String {
        var expanded = [CChar](repeating: 0, count: Int(PATH_MAX))
        return shown.withCString { input in
            guard let result = pscalPathVirtualizedHostPathC(input, &expanded) else { return shown }
            return String(cString: result)
        }
    }

    /// What the shell calls a real path.
    static func shownPath(_ host: String) -> String {
        var stripped = [CChar](repeating: 0, count: Int(PATH_MAX))
        let ok = host.withCString { pathTruncateStripC($0, &stripped, stripped.count) }
        return ok ? String(cString: stripped) : host
    }

    /// The real location of a path, for a framework that opens files itself. There's nothing
    /// to copy out: the app's files are already real files.
    static func fileURL(_ shown: String) -> URL {
        URL(fileURLWithPath: hostPath(shown))
    }

    static func parent(of path: String) -> String {
        let parent = (path as NSString).deletingLastPathComponent
        return parent.isEmpty ? "/" : parent
    }

    static func join(_ directory: String, _ name: String) -> String {
        (directory as NSString).appendingPathComponent(name)
    }

    // MARK: - Running

    private func run<Value>(_ queue: DispatchQueue,
                            _ operation: WorkspaceFileOperation,
                            work: @escaping @Sendable () throws -> Value,
                            completion: @escaping @MainActor (Result<Value, Error>) -> Void) {
        queue.async {
            let result: Result<Value, Error>
            if operation.isCancelled {
                result = .failure(WorkspaceFileError.cancelled)
            } else {
                result = Result(catching: work)
            }
            nonisolated(unsafe) let delivered = result
            DispatchQueue.main.async {
                MainActor.assumeIsolated {
                    completion(delivered)
                }
            }
        }
    }

    private static func item(atHostPath host: String, shownPath: String, fileManager: FileManager = .default) throws -> WorkspaceFileItem {
        var link = Darwin.stat()
        guard lstat(host, &link) == 0 else {
            throw WorkspaceFileError.system(errno)
        }
        var info = link
        var target: String?
        var broken = false
        if link.st_mode & S_IFMT == S_IFLNK {
            target = try? fileManager.destinationOfSymbolicLink(atPath: host)
            if stat(host, &info) != 0 {
                info = link
                broken = true
            }
        }
        return WorkspaceFileItem(name: (shownPath as NSString).lastPathComponent,
                                 path: shownPath,
                                 kind: broken ? .symlink : kind(ofMode: info.st_mode),
                                 size: UInt64(max(0, info.st_size)),
                                 modified: Date(timeIntervalSince1970: TimeInterval(info.st_mtimespec.tv_sec)),
                                 posixMode: info.st_mode,
                                 symlinkTarget: target)
    }

    private static func kind(ofMode mode: mode_t) -> WorkspaceFileKind {
        switch mode & S_IFMT {
        case S_IFREG: return .regular
        case S_IFDIR: return .directory
        case S_IFLNK: return .symlink
        case S_IFIFO: return .fifo
        case S_IFSOCK: return .socket
        case S_IFCHR: return .charDevice
        case S_IFBLK: return .blockDevice
        default: return .other
        }
    }

    private static func requirePath(_ path: String) throws {
        // An empty path must never reach a delete: an empty string can expand to a root.
        if path.isEmpty {
            throw WorkspaceFileError.emptyPath
        }
    }

    // MARK: - Directory and metadata

    /// In the filesystem's order, stably sorted by name for determinism only. How the File
    /// Manager sorts them is its own business.
    @discardableResult
    func listDirectory(_ path: String, completion: @escaping @MainActor (Result<[WorkspaceFileItem], Error>) -> Void) -> WorkspaceFileOperation {
        let operation = WorkspaceFileOperation()
        run(interactive, operation, work: {
            try Self.requirePath(path)
            let host = Self.hostPath(path)
            guard let directory = opendir(host) else {
                throw WorkspaceFileError.system(errno)
            }
            defer { closedir(directory) }
            var items: [WorkspaceFileItem] = []
            while let entry = readdir(directory) {
                if operation.isCancelled {
                    throw WorkspaceFileError.cancelled
                }
                let name = withUnsafePointer(to: &entry.pointee.d_name) { pointer in
                    pointer.withMemoryRebound(to: CChar.self, capacity: Int(NAME_MAX) + 1) { String(cString: $0) }
                }
                if name == "." || name == ".." {
                    continue
                }
                // One entry deleted between the listing and its stat isn't a failed listing.
                if let item = try? Self.item(atHostPath: Self.join(host, name), shownPath: Self.join(path, name)) {
                    items.append(item)
                }
            }
            return items.sorted { $0.name < $1.name }
        }, completion: completion)
        return operation
    }

    func item(_ path: String, completion: @escaping @MainActor (Result<WorkspaceFileItem, Error>) -> Void) {
        run(interactive, WorkspaceFileOperation(), work: {
            try Self.requirePath(path)
            return try Self.item(atHostPath: Self.hostPath(path), shownPath: path)
        }, completion: completion)
    }

    /// Free and total bytes of the filesystem holding `path`.
    func filesystemStatus(_ path: String, completion: @escaping @MainActor (Result<(available: Int64, total: Int64), Error>) -> Void) {
        run(interactive, WorkspaceFileOperation(), work: {
            let attributes = try FileManager.default.attributesOfFileSystem(forPath: Self.hostPath(path))
            let free = (attributes[.systemFreeSize] as? NSNumber)?.int64Value ?? 0
            let total = (attributes[.systemSize] as? NSNumber)?.int64Value ?? 0
            return (free, total)
        }, completion: completion)
    }

    // MARK: - Reading

    /// A whole file, up to `maxBytes`. Larger fails rather than silently truncating, and there
    /// is no read without a cap.
    @discardableResult
    func readFile(_ path: String, maxBytes: Int, completion: @escaping @MainActor (Result<Data, Error>) -> Void) -> WorkspaceFileOperation {
        let operation = WorkspaceFileOperation()
        run(maxBytes <= Self.interactiveLimit ? interactive : bulk, operation, work: {
            try Self.requirePath(path)
            let host = Self.hostPath(path)
            var info = Darwin.stat()
            guard stat(host, &info) == 0 else {
                throw WorkspaceFileError.system(errno)
            }
            guard info.st_mode & S_IFMT == S_IFREG else {
                throw WorkspaceFileError.notRegularFile
            }
            guard info.st_size <= maxBytes else {
                throw WorkspaceFileError.tooLarge
            }
            return try Data(contentsOf: URL(fileURLWithPath: host))
        }, completion: completion)
        return operation
    }

    // MARK: - Writing

    /// Atomic. Writing through a symlink updates the target rather than replacing the link,
    /// and a FIFO, socket or device is refused rather than clobbered.
    func write(_ data: Data, to path: String, completion: @escaping @MainActor (Result<Void, Error>) -> Void) {
        run(data.count <= Self.interactiveLimit ? interactive : bulk, WorkspaceFileOperation(), work: {
            try Self.requirePath(path)
            var host = Self.hostPath(path)
            if let target = try? FileManager.default.destinationOfSymbolicLink(atPath: host) {
                host = target.hasPrefix("/") ? target : Self.join(Self.parent(of: host), target)
            }
            var info = Darwin.stat()
            let exists = stat(host, &info) == 0
            if exists && info.st_mode & S_IFMT != S_IFREG {
                throw WorkspaceFileError.notRegularFile
            }
            try data.write(to: URL(fileURLWithPath: host), options: .atomic)
            // An atomic write replaces the file, which would otherwise reset its mode.
            if exists {
                chmod(host, info.st_mode & 0o7777)
            }
        }, completion: completion)
    }

    func createDirectory(_ path: String, completion: @escaping @MainActor (Result<Void, Error>) -> Void) {
        run(interactive, WorkspaceFileOperation(), work: {
            try Self.requirePath(path)
            if mkdir(Self.hostPath(path), 0o755) != 0 {
                throw WorkspaceFileError.system(errno)
            }
        }, completion: completion)
    }

    /// Doesn't replace what's already at the destination. A move across filesystems, which
    /// no single rename can do, copies and then deletes.
    @discardableResult
    func move(_ source: String, to destination: String, completion: @escaping @MainActor (Result<Void, Error>) -> Void) -> WorkspaceFileOperation {
        let operation = WorkspaceFileOperation()
        run(bulk, operation, work: {
            try Self.requirePath(source)
            try Self.requirePath(destination)
            try Self.requireNotMountTarget(source)
            let from = Self.hostPath(source)
            let to = Self.hostPath(destination)
            if access(to, F_OK) == 0 && from != to {
                throw WorkspaceFileError.system(EEXIST)
            }
            if rename(from, to) == 0 {
                return
            }
            let code = errno
            guard code == EXDEV else {
                throw WorkspaceFileError.system(code)
            }
            try FileManager.default.copyItem(atPath: from, toPath: to)
            try FileManager.default.removeItem(atPath: from)
        }, completion: completion)
        return operation
    }

    /// A file, or a folder and all it holds. Symlinks copy as links.
    @discardableResult
    func copy(_ source: String, to destination: String, completion: @escaping @MainActor (Result<Void, Error>) -> Void) -> WorkspaceFileOperation {
        let operation = WorkspaceFileOperation()
        run(bulk, operation, work: {
            try Self.requirePath(source)
            try Self.requirePath(destination)
            try FileManager.default.copyItem(atPath: Self.hostPath(source), toPath: Self.hostPath(destination))
        }, completion: completion)
        return operation
    }

    /// Where the `mount` command put folders, as the shell sees them.
    static func mountTargets() -> [String] {
        guard let list = pscalRuntimeCopyMountTargets() else { return [] }
        defer { free(list) }
        return String(cString: list).split(separator: "\n").map(String.init).sorted()
    }

    /// A mount target's host path is the folder that was picked, so removing or moving the
    /// target would act on the person's own folder rather than on the mount.
    private static func requireNotMountTarget(_ path: String) throws {
        if mountTargets().contains((path as NSString).standardizingPath) {
            throw WorkspaceFileError.refusedMount
        }
    }

    /// The app's own folders and anything above them can't be removed, whatever the path says.
    private static func isProtected(host: String) -> Bool {
        let path = (host as NSString).standardizingPath
        var protected = [NSHomeDirectory()]
        if let documents = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask).first {
            protected.append(documents.path)
            protected.append(documents.appendingPathComponent("home", isDirectory: true).path)
        }
        return path == "/" || protected.contains { ($0 as NSString).standardizingPath.hasPrefix(path + "/") || ($0 as NSString).standardizingPath == path }
    }

    /// A file, an empty folder, or with `recursive` a whole tree. Symlinks are never followed:
    /// a linked folder inside the tree is unlinked as itself.
    func remove(_ path: String, recursive: Bool, completion: @escaping @MainActor (Result<Void, Error>) -> Void) {
        run(recursive ? bulk : interactive, WorkspaceFileOperation(), work: {
            try Self.requirePath(path)
            try Self.requireNotMountTarget(path)
            let host = Self.hostPath(path)
            if Self.isProtected(host: host) {
                throw WorkspaceFileError.refusedRoot
            }
            if recursive {
                try FileManager.default.removeItem(atPath: host)
            } else if Darwin.remove(host) != 0 {
                throw WorkspaceFileError.system(errno)
            }
        }, completion: completion)
    }
}
