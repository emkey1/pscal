// Copyright 2026 Mike Miller
// SPDX-License-Identifier: Apache-2.0
//
// Ported by its author from iSH-AOK's Workspace (app/MotePadDocumentStore.m at 0f75fee1c)
// and licensed to PSCAL under the Apache License 2.0.

import Foundation

enum WorkspaceMotePadEntryKind {
    case file, directory, other
}

/// One row of the Open and Save As browser. A symlink is classified by its target, so a linked
/// folder can be entered. Anything else, a FIFO or a broken link, can be seen but not opened.
struct WorkspaceMotePadEntry {
    let name: String
    /// As the shell sees it.
    let path: String
    let kind: WorkspaceMotePadEntryKind
}

/// Which version of a file this is. The inode counts as well as the time and size, because an
/// atomic save by another program replaces the file.
struct WorkspaceMotePadFingerprint: Equatable {
    let seconds: Int64
    let nanoseconds: Int64
    let size: Int64
    let inode: Int64

    fileprivate init(_ info: Darwin.stat) {
        seconds = Int64(info.st_mtimespec.tv_sec)
        nanoseconds = Int64(info.st_mtimespec.tv_nsec)
        size = Int64(info.st_size)
        inode = Int64(truncatingIfNeeded: info.st_ino)
    }

    fileprivate init?(plist: [String: Any]) {
        guard let seconds = (plist["seconds"] as? NSNumber)?.int64Value,
              let nanoseconds = (plist["nanoseconds"] as? NSNumber)?.int64Value,
              let size = (plist["size"] as? NSNumber)?.int64Value,
              let inode = (plist["inode"] as? NSNumber)?.int64Value else { return nil }
        self.seconds = seconds
        self.nanoseconds = nanoseconds
        self.size = size
        self.inode = inode
    }

    fileprivate var plist: [String: Any] {
        ["seconds": seconds, "nanoseconds": nanoseconds, "size": size, "inode": inode]
    }
}

/// What's at a document's path now. `unknown` is a failure that says nothing about the file,
/// such as a folder that can't be searched, so it never reads as "deleted".
enum WorkspaceMotePadDiskReading: Equatable {
    case present(WorkspaceMotePadFingerprint)
    case missing
    case unknown
}

struct WorkspaceMotePadDocument {
    let text: String
    /// Kept out of the text and put back on save, so a file comes back byte for byte.
    let hasByteOrderMark: Bool
    let fingerprint: WorkspaceMotePadFingerprint?
}

/// A window's unsaved text, mirrored outside the file it shadows.
struct WorkspaceMotePadDraft {
    let text: String
    let path: String?
    let date: Date
    let hasByteOrderMark: Bool
    /// The file as it was when the document last matched it.
    let base: WorkspaceMotePadFingerprint?

    init(text: String, path: String?, date: Date, hasByteOrderMark: Bool, base: WorkspaceMotePadFingerprint?) {
        self.text = text
        self.path = path
        self.date = date
        self.hasByteOrderMark = hasByteOrderMark
        self.base = base
    }

    /// A corrupt or partial draft is nil: start clean, never a crash at launch.
    fileprivate init?(plistData: Data) {
        guard let plist = try? PropertyListSerialization.propertyList(from: plistData, options: [], format: nil) as? [String: Any],
              let text = plist["text"] as? String,
              (plist["dirty"] as? NSNumber)?.boolValue == true else { return nil }
        self.text = text
        path = plist["path"] as? String
        date = plist["date"] as? Date ?? .distantPast
        hasByteOrderMark = (plist["byteOrderMark"] as? NSNumber)?.boolValue ?? false
        base = (plist["base"] as? [String: Any]).flatMap(WorkspaceMotePadFingerprint.init(plist:))
    }

    fileprivate var plistData: Data? {
        var plist: [String: Any] = ["text": text, "dirty": true, "date": date, "byteOrderMark": hasByteOrderMark]
        if let path {
            plist["path"] = path
        }
        if let base {
            plist["base"] = base.plist
        }
        return try? PropertyListSerialization.data(fromPropertyList: plist, format: .binary, options: 0)
    }
}

enum WorkspaceMotePadError: LocalizedError {
    case tooLarge
    case notText
    case readOnly

    var errorDescription: String? {
        switch self {
        case .tooLarge:
            return "This file is too large for MotePad, which opens text files up to \(WorkspaceMotePadStore.maxFileBytes / (1024 * 1024)) MB."
        case .notText:
            return "This file isn't UTF-8 text. MotePad won't open it, because saving it again would change its bytes."
        case .readOnly:
            return "This file is read-only."
        }
    }
}

/// MotePad's files: documents through `WorkspaceFileService`, and the autosaved drafts in the
/// app's Application Support folder, where the shell doesn't see them. Every operation runs off
/// the main thread and completes on it, in the order it was asked for.
final class WorkspaceMotePadStore: @unchecked Sendable {
    static let shared = WorkspaceMotePadStore()
    /// Past this a text view is too slow to use. It's also where the file service's interactive
    /// lane ends, so a read and a save of one file stay on one serial queue.
    static let maxFileBytes = 8 * 1024 * 1024
    private static let byteOrderMark: [UInt8] = [0xEF, 0xBB, 0xBF]
    private static let draftSlotLimit = 64

    private let files = WorkspaceFileService.shared
    private let queue = DispatchQueue(label: "com.pscal.workspace.motepad.io", qos: .userInitiated)
    private let slotLock = NSLock()
    private var claimedSlots = Set<String>()

    private static func deliver<Value>(_ value: Value, to completion: @escaping @MainActor (Value) -> Void) {
        nonisolated(unsafe) let delivered = value
        DispatchQueue.main.async {
            MainActor.assumeIsolated {
                completion(delivered)
            }
        }
    }

    // MARK: - Paths

    /// Without a trailing slash, so path arithmetic is stable.
    static func normalizedPath(_ path: String) -> String {
        var normalized = path.hasPrefix("/") ? path : "/" + path
        while normalized.count > 1 && normalized.hasSuffix("/") {
            normalized.removeLast()
        }
        return normalized
    }

    /// Where the browser starts for an untitled document: the shell's home, in the app's own
    /// space, or the closest thing there is to it.
    func defaultDirectory(completion: @escaping @MainActor (String) -> Void) {
        queue.async {
            var directory = "/"
            if let documents = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask).first {
                let home = documents.appendingPathComponent("home", isDirectory: true)
                var isDirectory: ObjCBool = false
                let exists = FileManager.default.fileExists(atPath: home.path, isDirectory: &isDirectory)
                directory = WorkspaceFileService.shownPath(exists && isDirectory.boolValue ? home.path : documents.path)
            }
            Self.deliver(directory, to: completion)
        }
    }

    // MARK: - Documents

    /// Folders first, then files, each sorted without regard to case.
    func listDirectory(_ path: String, completion: @escaping @MainActor (Result<[WorkspaceMotePadEntry], Error>) -> Void) {
        files.listDirectory(path) { result in
            self.queue.async {
                let entries = result.map { items -> [WorkspaceMotePadEntry] in
                    let mapped = items.map { item -> WorkspaceMotePadEntry in
                        let kind: WorkspaceMotePadEntryKind
                        switch item.kind {
                        case .regular: kind = .file
                        case .directory: kind = .directory
                        default: kind = .other
                        }
                        return WorkspaceMotePadEntry(name: item.name, path: item.path, kind: kind)
                    }
                    return mapped.sorted { lhs, rhs in
                        if (lhs.kind == .directory) != (rhs.kind == .directory) {
                            return lhs.kind == .directory
                        }
                        return lhs.name.caseInsensitiveCompare(rhs.name) == .orderedAscending
                    }
                }
                Self.deliver(entries, to: completion)
            }
        }
    }

    /// A file too large or not UTF-8 fails with its reason. There's no lossy decode: a document
    /// that opens has to save back as the same bytes.
    func readText(_ path: String, completion: @escaping @MainActor (Result<WorkspaceMotePadDocument, Error>) -> Void) {
        let path = Self.normalizedPath(path)
        queue.async {
            // Before the read, so a change during it shows up afterwards as a change.
            var fingerprint: WorkspaceMotePadFingerprint?
            if case .present(let current) = Self.reading(atPath: path) {
                fingerprint = current
            }
            let known = fingerprint
            self.files.readFile(path, maxBytes: Self.maxFileBytes) { result in
                self.queue.async {
                    let decoded = Result { () throws -> WorkspaceMotePadDocument in
                        do {
                            return try Self.decode(result.get(), fingerprint: known)
                        } catch WorkspaceFileError.tooLarge {
                            throw WorkspaceMotePadError.tooLarge
                        }
                    }
                    Self.deliver(decoded, to: completion)
                }
            }
        }
    }

    private static func decode(_ data: Data, fingerprint: WorkspaceMotePadFingerprint?) throws -> WorkspaceMotePadDocument {
        let hasMark = data.starts(with: byteOrderMark)
        let body = hasMark ? data.dropFirst(byteOrderMark.count) : data
        // A NUL is valid UTF-8, but it marks a binary file and a text view can't show it.
        guard !body.contains(0), let text = String(data: body, encoding: .utf8) else {
            throw WorkspaceMotePadError.notText
        }
        return WorkspaceMotePadDocument(text: text, hasByteOrderMark: hasMark, fingerprint: fingerprint)
    }

    /// Atomic, and refuses a read-only file: the rename behind an atomic write would otherwise
    /// replace one whatever its mode says. The answer is the file as it is after the write.
    func writeText(_ text: String,
                   hasByteOrderMark: Bool,
                   to path: String,
                   completion: @escaping @MainActor (Result<WorkspaceMotePadFingerprint?, Error>) -> Void) {
        let path = Self.normalizedPath(path)
        queue.async {
            let host = WorkspaceFileService.hostPath(path)
            if access(host, F_OK) == 0, access(host, W_OK) != 0 {
                Self.deliver(.failure(WorkspaceMotePadError.readOnly), to: completion)
                return
            }
            let body = Data(text.utf8)
            let data = hasByteOrderMark ? Data(Self.byteOrderMark) + body : body
            self.files.write(data, to: path) { result in
                self.queue.async {
                    let written = result.map { () -> WorkspaceMotePadFingerprint? in
                        if case .present(let current) = Self.reading(atPath: path) {
                            return current
                        }
                        return nil
                    }
                    Self.deliver(written, to: completion)
                }
            }
        }
    }

    func diskReading(_ path: String, completion: @escaping @MainActor (WorkspaceMotePadDiskReading) -> Void) {
        queue.async {
            Self.deliver(Self.reading(atPath: path), to: completion)
        }
    }

    /// Follows a symlink, because a save writes through one to its target.
    private static func reading(atPath path: String) -> WorkspaceMotePadDiskReading {
        var info = Darwin.stat()
        if stat(WorkspaceFileService.hostPath(path), &info) == 0 {
            return .present(WorkspaceMotePadFingerprint(info))
        }
        return errno == ENOENT || errno == ENOTDIR ? .missing : .unknown
    }

    // MARK: - Drafts

    /// The workspace can hold several MotePad windows, so each claims a numbered draft of its
    /// own. Too many windows gets nil, and that one goes without autosave rather than share.
    func claimDraftSlot() -> String? {
        slotLock.lock()
        defer { slotLock.unlock() }
        for slot in 0..<Self.draftSlotLimit {
            let name = "draft-\(slot).plist"
            if claimedSlots.insert(name).inserted {
                return name
            }
        }
        return nil
    }

    /// False if another window holds it. The name comes from a saved layout, so it's checked.
    func claimDraftSlot(named name: String) -> Bool {
        guard name.hasPrefix("draft-"), !name.contains("/") else { return false }
        slotLock.lock()
        defer { slotLock.unlock() }
        return claimedSlots.insert(name).inserted
    }

    /// Safe from any thread, because a view controller's deinit calls it.
    func releaseDraftSlot(_ name: String) {
        slotLock.lock()
        claimedSlots.remove(name)
        slotLock.unlock()
    }

    private static func draftURL(slot: String) -> URL? {
        guard let base = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask).first else { return nil }
        let directory = base.appendingPathComponent("MotePadDrafts", isDirectory: true)
        try? FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        return directory.appendingPathComponent(slot, isDirectory: false)
    }

    /// A slot's draft and what's at its document's path now, or nil if there's no usable draft.
    func loadDraft(slot: String,
                   completion: @escaping @MainActor ((draft: WorkspaceMotePadDraft, reading: WorkspaceMotePadDiskReading)?) -> Void) {
        queue.async {
            var loaded: (draft: WorkspaceMotePadDraft, reading: WorkspaceMotePadDiskReading)?
            if let url = Self.draftURL(slot: slot),
               let data = try? Data(contentsOf: url),
               let draft = WorkspaceMotePadDraft(plistData: data) {
                loaded = (draft, draft.path.map(Self.reading(atPath:)) ?? .missing)
            }
            Self.deliver(loaded, to: completion)
        }
    }

    func writeDraft(_ draft: WorkspaceMotePadDraft, slot: String, completion: (@MainActor () -> Void)? = nil) {
        queue.async {
            if let url = Self.draftURL(slot: slot), let data = draft.plistData {
                try? data.write(to: url, options: .atomic)
            }
            if let completion {
                Self.deliver((), to: { _ in completion() })
            }
        }
    }

    func removeDraft(slot: String) {
        queue.async {
            if let url = Self.draftURL(slot: slot) {
                try? FileManager.default.removeItem(at: url)
            }
        }
    }
}
