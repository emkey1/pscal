// Copyright 2026 Mike Miller
// SPDX-License-Identifier: Apache-2.0
//
// Ported by its author from iSH-AOK's Workspace (app/AudioLibrary.m at 0f75fee1c) and licensed
// to PSCAL under the Apache License 2.0.

import Foundation

// The player's way to the filesystem: finding audio under a folder, and the playlists. Files
// here are real files, so nothing is extracted and a track is just its path.

extension Notification.Name {
    /// A folder scan started or finished. Posted on the main thread.
    static let workspaceAudioLibraryDidChange = Notification.Name("WorkspaceAudioLibraryDidChange")
}

/// What the player tries. AVFoundation decides in the end: Ogg and Opus only play where it can
/// read them by itself, and a file it can't read gets a message and is moved past.
private let workspaceAudioExtensions: Set<String> = [
    "mp3", "m4a", "mp4", "aac", "adts", "alac", "wav", "wave", "aif", "aiff", "aifc", "caf", "flac",
    "ogg", "oga", "opus",
]

/// The tracks under `hostRoot`, sorted by path and named from `shownRoot`, or nil if cancelled.
/// Symlinks aren't followed except the root itself, so a loop or a link out of the tree counts
/// for nothing, and hidden files and folders are skipped. A file as the root is itself the result.
private func workspaceAudioWalk(hostRoot: String, shownRoot: String, operation: WorkspaceFileOperation) -> [WorkspaceAudioTrack]? {
    var tracks: [WorkspaceAudioTrack] = []
    guard let root = strdup(hostRoot) else { return tracks }
    defer { free(root) }
    let rootLength = strlen(root)
    let roots: [UnsafeMutablePointer<CChar>?] = [root, nil]
    // FTS_NOCHDIR because a process with other threads can't change directory under them.
    guard let walk = fts_open(roots, FTS_PHYSICAL | FTS_COMFOLLOW | FTS_NOCHDIR, nil) else { return tracks }
    defer { fts_close(walk) }
    var sinceCheck = 0
    while let entry = fts_read(walk) {
        sinceCheck += 1
        if sinceCheck >= 256 {
            sinceCheck = 0
            if operation.isCancelled {
                return nil
            }
        }
        let info = Int32(entry.pointee.fts_info)
        guard info == FTS_D || info == FTS_F else { continue }
        var relativePointer = UnsafePointer<CChar>(entry.pointee.fts_path + rootLength)
        while relativePointer.pointee == CChar(UInt8(ascii: "/")) {
            relativePointer += 1
        }
        let relative = String(cString: relativePointer)
        let name = (relative as NSString).lastPathComponent
        if info == FTS_D {
            if entry.pointee.fts_level > 0 && name.hasPrefix(".") {
                fts_set(walk, entry, FTS_SKIP)
            }
            continue
        }
        if relative.isEmpty {
            if WorkspaceAudioLibrary.isSupportedAudioFileName(shownRoot) {
                tracks.append(WorkspaceAudioTrack(path: shownRoot))
            }
        } else if !name.hasPrefix("."), WorkspaceAudioLibrary.isSupportedAudioFileName(name) {
            tracks.append(WorkspaceAudioTrack(path: WorkspaceFileService.join(shownRoot, relative)))
        }
    }
    if operation.isCancelled {
        return nil
    }
    tracks.sort { $0.path.caseInsensitiveCompare($1.path) == .orderedAscending }
    return tracks
}

/// Finds audio and keeps playlists. Scans run off the main thread and stop when cancelled; a
/// playlist is a JSON file under the music folder, where the shell can see and copy it.
@MainActor
final class WorkspaceAudioLibrary {
    static let shared = WorkspaceAudioLibrary()

    /// What the interface calls the default folder.
    static let musicFolderDisplayName = "~/Music"

    private var scans: [WorkspaceFileOperation] = []
    private var openGeneration = 0
    // One at a time, so a save finishes before a list of what's saved.
    private let playlistQueue = DispatchQueue(label: "com.pscal.workspace.audio.playlists", qos: .userInitiated)

    private init() {}

    var isScanning: Bool { !scans.isEmpty }

    /// True if `name` ends in an extension the player tries. Any thread.
    nonisolated static func isSupportedAudioFileName(_ name: String) -> Bool {
        workspaceAudioExtensions.contains((name as NSString).pathExtension.lowercased())
    }

    // MARK: - Scanning

    /// The tracks in a folder and everything under it, or the track a file is. Completes with
    /// nil if cancelled, by the returned operation or `cancelScans()`.
    @discardableResult
    func scan(path: String, completion: @escaping @MainActor ([WorkspaceAudioTrack]?) -> Void) -> WorkspaceFileOperation {
        let operation = WorkspaceFileOperation()
        scans.append(operation)
        NotificationCenter.default.post(name: .workspaceAudioLibraryDidChange, object: self)
        DispatchQueue.global(qos: .userInitiated).async {
            // An empty path must never go further: an empty string can expand to a root.
            let tracks = path.isEmpty ? [] : workspaceAudioWalk(hostRoot: WorkspaceFileService.hostPath(path), shownRoot: path, operation: operation)
            DispatchQueue.main.async {
                MainActor.assumeIsolated {
                    self.scanFinished(operation)
                    completion(operation.isCancelled ? nil : tracks)
                }
            }
        }
        return operation
    }

    private func scanFinished(_ operation: WorkspaceFileOperation) {
        scans.removeAll { $0 === operation }
        NotificationCenter.default.post(name: .workspaceAudioLibraryDidChange, object: self)
    }

    func cancelScans() {
        scans.forEach { $0.cancel() }
    }

    /// Scans the default music folder, which is created if it isn't there.
    func scanMusicFolder(completion: @escaping @MainActor ([WorkspaceAudioTrack]?) -> Void) {
        DispatchQueue.global(qos: .userInitiated).async {
            let path = Self.preparedFolder(Self.musicFolderPath())
            DispatchQueue.main.async {
                MainActor.assumeIsolated {
                    _ = self.scan(path: path, completion: completion)
                }
            }
        }
    }

    /// Queues the audio in a folder without disturbing what's playing: File Manager's "Add Folder
    /// to Music". Completes with how many tracks were added, 0 if it found none, nil if cancelled.
    @discardableResult
    func addFolder(atPath path: String, completion: (@MainActor (Int?) -> Void)? = nil) -> WorkspaceFileOperation {
        scan(path: path) { tracks in
            if let tracks {
                let name = (path as NSString).lastPathComponent
                WorkspaceAudioEngine.shared.enqueue(tracks)
                switch tracks.count {
                case 0: WorkspaceAudioEngine.shared.setStatus("“\(name)” doesn’t contain any audio files.")
                case 1: WorkspaceAudioEngine.shared.setStatus("Added 1 track to the queue.")
                default: WorkspaceAudioEngine.shared.setStatus("Added \(tracks.count) tracks to the queue.")
                }
            }
            completion?(tracks?.count)
        }
    }

    /// What giving Music a path does, as double-clicking does elsewhere: it replaces the queue and
    /// plays. A file is one track, whatever its name, and AVFoundation has the last word on it; a
    /// folder is the audio under it.
    func open(path: String) {
        openGeneration += 1
        let generation = openGeneration
        WorkspaceFileService.shared.item(path) { [weak self] result in
            guard let self, self.openGeneration == generation else { return }
            guard case .success(let item) = result, item.kind == .directory else {
                // A file that's gone gets the player's own message about it.
                WorkspaceAudioEngine.shared.setQueue([WorkspaceAudioTrack(path: path)], startIndex: 0)
                return
            }
            self.scan(path: path) { [weak self] tracks in
                guard let self, self.openGeneration == generation else { return }
                guard let tracks else {
                    WorkspaceAudioEngine.shared.setStatus("Scan cancelled.")
                    return
                }
                if tracks.isEmpty {
                    WorkspaceAudioEngine.shared.setStatus("“\(item.name)” doesn’t contain any audio files.")
                } else {
                    WorkspaceAudioEngine.shared.setQueue(tracks, startIndex: 0)
                }
            }
        }
    }

    // MARK: - Paths

    /// The shell's home. Until the runtime has started, HOME is still the system's, the container.
    private nonisolated static func homeShownPath() -> String {
        let documents = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask).first?.path
            ?? (NSHomeDirectory() as NSString).appendingPathComponent("Documents")
        var host = (documents as NSString).appendingPathComponent("home")
        if let value = getenv("HOME") {
            let home = String(cString: value)
            if !home.isEmpty && home != NSHomeDirectory() {
                host = home
            }
        }
        return WorkspaceFileService.shownPath(host)
    }

    private nonisolated static func musicFolderPath() -> String {
        WorkspaceFileService.join(homeShownPath(), "Music")
    }

    private nonisolated static func preparedFolder(_ path: String) -> String {
        try? FileManager.default.createDirectory(atPath: WorkspaceFileService.hostPath(path), withIntermediateDirectories: true)
        return path
    }

    /// A path someone typed: `~` is their home, and anything else without a leading slash is
    /// under it. Any thread.
    nonisolated static func resolveTypedPath(_ typed: String) -> String {
        if typed == "~" {
            return homeShownPath()
        }
        if typed.hasPrefix("~/") {
            return WorkspaceFileService.join(homeShownPath(), String(typed.dropFirst(2)))
        }
        return typed.hasPrefix("/") ? typed : WorkspaceFileService.join(homeShownPath(), typed)
    }

    // MARK: - Playlists

    /// A name's file name. Everything a name can hold survives, in any script, except what a path
    /// can't: separators, control characters and a leading dot, which would hide the file.
    private nonisolated static func playlistFileName(_ name: String) -> String {
        var safe = ""
        for scalar in name.unicodeScalars {
            let refused = scalar == "/" || scalar == ":" || scalar == "\\" || CharacterSet.controlCharacters.contains(scalar)
            safe.unicodeScalars.append(refused ? "_" : scalar)
        }
        var trimmed = safe.trimmingCharacters(in: .whitespacesAndNewlines)
        while trimmed.hasPrefix(".") {
            trimmed.removeFirst()
        }
        // A file name is at most 255 bytes, and these go with ".json".
        while trimmed.utf8.count > 240 {
            trimmed.removeLast()
        }
        return trimmed.isEmpty ? "Playlist" : trimmed
    }

    private nonisolated static func playlistsHostFolder() -> String {
        let shown = preparedFolder(WorkspaceFileService.join(musicFolderPath(), "Playlists"))
        return WorkspaceFileService.hostPath(shown)
    }

    private nonisolated static func playlistHostPath(_ name: String) -> String {
        WorkspaceFileService.join(playlistsHostFolder(), playlistFileName(name) + ".json")
    }

    private nonisolated func finish<Value: Sendable>(_ value: Value, _ completion: @escaping @MainActor (Value) -> Void) {
        DispatchQueue.main.async {
            MainActor.assumeIsolated {
                completion(value)
            }
        }
    }

    func playlistNames(completion: @escaping @MainActor ([String]) -> Void) {
        playlistQueue.async { [self] in
            let files = (try? FileManager.default.contentsOfDirectory(atPath: Self.playlistsHostFolder())) ?? []
            let names = files
                .filter { !$0.hasPrefix(".") && ($0 as NSString).pathExtension.lowercased() == "json" }
                .map { ($0 as NSString).deletingPathExtension }
                .sorted { $0.caseInsensitiveCompare($1) == .orderedAscending }
            finish(names, completion)
        }
    }

    /// Empty if there's no such playlist or it can't be read.
    func tracks(forPlaylist name: String, completion: @escaping @MainActor ([WorkspaceAudioTrack]) -> Void) {
        playlistQueue.async { [self] in
            var tracks: [WorkspaceAudioTrack] = []
            if let data = FileManager.default.contents(atPath: Self.playlistHostPath(name)),
               let entries = try? JSONSerialization.jsonObject(with: data) as? [[String: Any]] {
                tracks = entries.compactMap { WorkspaceAudioTrack(dictionary: $0) }
            }
            finish(tracks, completion)
        }
    }

    /// Completes with whether it's saved.
    func savePlaylist(named name: String, tracks: [WorkspaceAudioTrack], completion: (@MainActor (Bool) -> Void)? = nil) {
        playlistQueue.async { [self] in
            var saved = false
            if let data = try? JSONSerialization.data(withJSONObject: tracks.map(\.dictionaryRepresentation), options: .prettyPrinted) {
                saved = (try? data.write(to: URL(fileURLWithPath: Self.playlistHostPath(name)), options: .atomic)) != nil
            }
            if let completion {
                finish(saved, completion)
            }
        }
    }

    func deletePlaylist(named name: String, completion: (@MainActor () -> Void)? = nil) {
        playlistQueue.async { [self] in
            try? FileManager.default.removeItem(atPath: Self.playlistHostPath(name))
            if let completion {
                finish((), completion)
            }
        }
    }
}
