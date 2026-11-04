// Copyright 2026 Mike Miller
// SPDX-License-Identifier: Apache-2.0
//
// Ported by its author from iSH-AOK's Workspace (app/WorkspaceVideoPlayer.m at 0f75fee1c) and
// licensed to PSCAL under the Apache License 2.0.

import AVFoundation
import AVKit
import UIKit

/// The app has one audio session, which everything in it shares. A video holds it while it
/// plays, and on the last one stopping lets go and tells other apps they can have theirs back.
/// The calls can block on the media server, so they run on a queue of their own.
enum WorkspaceVideoAudioSession {
    private static let queue = DispatchQueue(label: "com.pscal.workspace.videoplayer.audiosession", qos: .userInitiated)
    // On the queue only.
    private nonisolated(unsafe) static var holders = 0

    static func acquire() {
        WorkspaceAudioSessionSharing.begin()
        queue.async {
            holders += 1
            guard holders == 1 else { return }
            let session = AVAudioSession.sharedInstance()
            // Changing the category under something already playing would disturb it.
            if !WorkspaceAudioSessionSharing.musicOrSDLIsPlaying {
                try? session.setCategory(.playback, mode: .moviePlayback)
            }
            try? session.setActive(true)
        }
    }

    static func release() {
        WorkspaceAudioSessionSharing.end()
        queue.async {
            holders -= 1
            guard holders == 0, !WorkspaceAudioSessionSharing.musicOrSDLIsPlaying else { return }
            try? AVAudioSession.sharedInstance().setActive(false, options: .notifyOthersOnDeactivation)
        }
    }
}

/// One line of a playlist: the name to show and where it is, as a URL or a shell path.
private struct WorkspaceVideoPlaylistEntry: Sendable {
    let title: String
    var location: String
}

private enum WorkspaceVideoPlaylist {
    /// Past this a playlist isn't one. IPTV lists run to tens of megabytes.
    static let maxBytes = 64 * 1024 * 1024

    static func isRemote(_ location: String) -> Bool {
        let lower = location.lowercased()
        return lower.hasPrefix("http://") || lower.hasPrefix("https://")
    }

    static func isPlaylistPath(_ path: String) -> Bool {
        let fileExtension = (path as NSString).pathExtension.lowercased()
        return fileExtension == "m3u" || fileExtension == "m3u8"
    }

    /// A remote address worth reading as a playlist before playing: one ending in .m3u or .m3u8,
    /// or an IPTV-style link that names the format in its query (get.php?...&type=m3u_plus).
    /// Anything else goes to AVPlayer as it is.
    static func looksLikePlaylist(_ url: URL) -> Bool {
        isPlaylistPath(url.path) || url.query?.lowercased().contains("m3u") == true
    }

    /// Before iOS 17 a URL can't hold a space or a non-ASCII character unescaped.
    static func webURL(_ text: String) -> URL? {
        if let url = URL(string: text) {
            return url
        }
        var allowed = CharacterSet.urlFragmentAllowed
        allowed.insert(charactersIn: "#%")
        return text.addingPercentEncoding(withAllowedCharacters: allowed).flatMap(URL.init(string:))
    }

    /// `rtsp://...` and the like, which no entry here can play.
    static func hasOtherScheme(_ location: String) -> Bool {
        guard !isRemote(location), let colon = location.firstIndex(of: ":") else { return false }
        let scheme = location[..<colon]
        return scheme.count > 1 && !scheme.contains("/") && scheme.allSatisfy { $0.isASCII && ($0.isLetter || $0.isNumber || "+-.".contains($0)) }
    }

    /// Text doesn't hold a NUL, and a stream does, soon.
    static func looksBinary(_ data: Data) -> Bool {
        !data.starts(with: [0xFF, 0xFE]) && !data.starts(with: [0xFE, 0xFF]) && data.prefix(4096).contains(0)
    }

    /// UTF-8 first, UTF-16 if it says so, and Latin-1 for the rest. nil for what isn't text.
    static func decode(_ data: Data) -> String? {
        if looksBinary(data) {
            return nil
        }
        if data.starts(with: [0xFF, 0xFE]) || data.starts(with: [0xFE, 0xFF]) {
            return String(data: data, encoding: .utf16)
        }
        return String(data: data, encoding: .utf8) ?? String(data: data, encoding: .isoLatin1)
    }

    /// Two things share the extensions. An HLS playlist (#EXT-X- tags) is one stream cut into
    /// segments, which AVPlayer plays from its URL. Any other .m3u is a list of streams or
    /// files, one per line, with optional "#EXTINF:<duration> <attributes>,<title>" lines naming
    /// the next one, the shape of IPTV channel lists. A line that can't be an address, holding
    /// control characters, is dropped, so a damaged file comes out short and not as nonsense.
    static func parse(_ source: String) -> (entries: [WorkspaceVideoPlaylistEntry], isHLS: Bool) {
        var text = Substring(source)
        if text.unicodeScalars.first == "\u{FEFF}" {
            text.unicodeScalars.removeFirst()
        }
        var entries: [WorkspaceVideoPlaylistEntry] = []
        var isHLS = false
        var pendingTitle: String?
        for raw in text.split(whereSeparator: \.isNewline) {
            let line = raw.trimmingCharacters(in: .whitespaces)
            if line.isEmpty {
                continue
            }
            if line.hasPrefix("#") {
                if line.hasPrefix("#EXT-X-") {
                    isHLS = true
                } else if line.hasPrefix("#EXTINF:") {
                    pendingTitle = title(ofInfoLine: line)
                }
                continue
            }
            if line.unicodeScalars.contains(where: { $0.value < 0x20 || $0.value == 0x7F }) {
                pendingTitle = nil
                continue
            }
            entries.append(WorkspaceVideoPlaylistEntry(title: pendingTitle ?? (line as NSString).lastPathComponent, location: line))
            pendingTitle = nil
        }
        return (entries, isHLS)
    }

    /// The title follows the first comma outside the attributes' quotes: tvg-name="News, Sport"
    /// must not end it early.
    private static func title(ofInfoLine line: String) -> String? {
        var quoted = false
        for index in line.indices {
            if line[index] == "\"" {
                quoted.toggle()
            } else if line[index] == "," && !quoted {
                let title = line[line.index(after: index)...].trimmingCharacters(in: .whitespaces)
                return title.isEmpty ? nil : title
            }
        }
        return nil
    }

    /// Where each entry is: a URL as written or resolved against a remote playlist's own address,
    /// or a path as written or against a local playlist's folder. An entry that can't be placed
    /// stays as written, and saying so is left for when it's chosen.
    static func resolved(_ entries: [WorkspaceVideoPlaylistEntry], source: String) -> [WorkspaceVideoPlaylistEntry] {
        let remote = isRemote(source)
        let base = remote ? webURL(source) : nil
        let folder = WorkspaceFileService.parent(of: source)
        return entries.map { entry in
            var entry = entry
            if isRemote(entry.location) || hasOtherScheme(entry.location) {
                return entry
            }
            if remote {
                if let resolved = URL(string: entry.location, relativeTo: base) ?? webURL(entry.location).flatMap({ URL(string: $0.absoluteString, relativeTo: base) }) {
                    entry.location = resolved.absoluteString
                }
            } else {
                entry.location = resolvedPath(entry.location, against: folder)
            }
            return entry
        }
    }

    /// By text alone, since the path is the shell's and not a real one: `..` stops at the root.
    private static func resolvedPath(_ link: String, against directory: String) -> String {
        let joined = link.hasPrefix("/") ? link : directory + "/" + link
        var parts: [Substring] = []
        for part in joined.split(separator: "/") {
            if part == ".." {
                _ = parts.popLast()
            } else if part != "." {
                parts.append(part)
            }
        }
        return "/" + parts.joined(separator: "/")
    }
}

/// Reads a remote playlist, up to a point. A link that looks like a playlist can answer with an
/// endless stream, and holding all of that would end with the system killing the app. So a
/// response that says it's media, or holds no text, or runs past the limit, is left to AVPlayer.
private final class WorkspaceVideoPlaylistFetcher: NSObject, URLSessionDataDelegate, @unchecked Sendable {
    enum Outcome: Sendable {
        case data(Data)
        case notAPlaylist
        case failure(String)
    }

    private let url: URL
    private let completion: @MainActor (Outcome) -> Void
    private var session: URLSession?
    // On the session's delegate queue only.
    private var received = Data()
    private var finished = false

    init(url: URL, completion: @escaping @MainActor (Outcome) -> Void) {
        self.url = url
        self.completion = completion
        super.init()
        let configuration = URLSessionConfiguration.default
        configuration.requestCachePolicy = .reloadIgnoringLocalCacheData
        configuration.timeoutIntervalForRequest = 30
        configuration.timeoutIntervalForResource = 120
        session = URLSession(configuration: configuration, delegate: self, delegateQueue: nil)
    }

    func start() {
        session?.dataTask(with: url).resume()
    }

    /// The session holds its delegate until this, so it's also what lets this object go.
    func cancel() {
        session?.invalidateAndCancel()
    }

    private func finish(_ outcome: Outcome) {
        guard !finished else { return }
        finished = true
        received = Data()
        session?.invalidateAndCancel()
        let completion = self.completion
        DispatchQueue.main.async {
            MainActor.assumeIsolated {
                completion(outcome)
            }
        }
    }

    func urlSession(_ session: URLSession, dataTask: URLSessionDataTask, didReceive response: URLResponse, completionHandler: @escaping @Sendable (URLSession.ResponseDisposition) -> Void) {
        if let http = response as? HTTPURLResponse, !(200..<300).contains(http.statusCode) {
            completionHandler(.cancel)
            finish(.failure("The server answered \(http.statusCode)."))
            return
        }
        let type = response.mimeType?.lowercased() ?? ""
        if type.hasPrefix("video/") || (type.hasPrefix("audio/") && !type.contains("mpegurl")) {
            completionHandler(.cancel)
            finish(.notAPlaylist)
            return
        }
        completionHandler(.allow)
    }

    func urlSession(_ session: URLSession, dataTask: URLSessionDataTask, didReceive data: Data) {
        guard !finished else { return }
        received.append(data)
        if received.count > WorkspaceVideoPlaylist.maxBytes || WorkspaceVideoPlaylist.looksBinary(received) {
            finish(.notAPlaylist)
        }
    }

    func urlSession(_ session: URLSession, task: URLSessionTask, didCompleteWithError error: Error?) {
        if let error {
            finish(.failure(error.localizedDescription))
        } else {
            finish(.data(received))
        }
    }
}

/// The channel list: every entry, filtered by what's typed in the search field.
private final class WorkspaceVideoPlaylistPicker: UITableViewController, UISearchResultsUpdating {
    private let entries: [WorkspaceVideoPlaylistEntry]
    private let onPick: (Int) -> Void
    private var shown: [Int] = []
    private let search = UISearchController(searchResultsController: nil)

    init(entries: [WorkspaceVideoPlaylistEntry], onPick: @escaping (Int) -> Void) {
        self.entries = entries
        self.onPick = onPick
        super.init(style: .plain)
    }

    @available(*, unavailable)
    required init?(coder: NSCoder) {
        fatalError("init(coder:) is not used")
    }

    override func viewDidLoad() {
        super.viewDidLoad()
        title = "Channels (\(entries.count))"
        tableView.accessibilityIdentifier = "workspace.videoplayer.channels.list"
        let close = UIBarButtonItem(barButtonSystemItem: .close, target: self, action: #selector(closeTapped))
        close.accessibilityIdentifier = "workspace.videoplayer.channels.close"
        navigationItem.rightBarButtonItem = close
        search.searchResultsUpdater = self
        search.obscuresBackgroundDuringPresentation = false
        search.searchBar.placeholder = "Search"
        search.searchBar.accessibilityIdentifier = "workspace.videoplayer.channels.search"
        navigationItem.searchController = search
        navigationItem.hidesSearchBarWhenScrolling = false
        definesPresentationContext = true
        filter("")
    }

    @objc private func closeTapped() {
        dismiss(animated: true)
    }

    private func filter(_ query: String) {
        shown = entries.indices.filter { query.isEmpty || entries[$0].title.localizedCaseInsensitiveContains(query) }
        tableView.reloadData()
    }

    func updateSearchResults(for searchController: UISearchController) {
        filter(searchController.searchBar.text ?? "")
    }

    override func tableView(_ tableView: UITableView, numberOfRowsInSection section: Int) -> Int {
        shown.count
    }

    override func tableView(_ tableView: UITableView, cellForRowAt indexPath: IndexPath) -> UITableViewCell {
        let cell = tableView.dequeueReusableCell(withIdentifier: "entry") ?? UITableViewCell(style: .subtitle, reuseIdentifier: "entry")
        let index = shown[indexPath.row]
        let entry = entries[index]
        cell.textLabel?.text = entry.title
        cell.detailTextLabel?.text = entry.title == entry.location ? nil : entry.location
        cell.detailTextLabel?.textColor = .secondaryLabel
        cell.accessibilityIdentifier = "workspace.videoplayer.channels.row.\(index)"
        return cell
    }

    override func tableView(_ tableView: UITableView, didSelectRowAt indexPath: IndexPath) {
        let index = shown[indexPath.row]
        let onPick = self.onPick
        presentingViewController?.dismiss(animated: true) {
            onPick(index)
        }
    }
}

/// A video player on AVPlayerViewController, which brings the scrubber, the rate, AirPlay and
/// full screen. A file plays straight from where it is and is never read into memory, however
/// big, and a web stream or an .m3u playlist opens from an address.
///
/// On another Desktop a video goes on playing, heard and not seen. Closing the window is what
/// ends it.
@MainActor
final class WorkspaceVideoPlayerAppletViewController: WorkspaceAppletViewController, WorkspaceFileOpenable, WorkspaceStatefulTool, WorkspaceFocusable {
    private static let fileQueue = DispatchQueue(label: "com.pscal.workspace.videoplayer.files", qos: .userInitiated)
    /// Scrubbing pauses and resumes, and a session handed back and forth would stutter.
    private static let sessionReleaseDelay: TimeInterval = 2

    private let playerViewController = AVPlayerViewController()
    private let statusLabel = UILabel()
    private let bar = UIStackView()
    private let openURLButton = UIButton(type: .system)
    private let channelsButton = UIButton(type: .system)

    /// The file playing, if it is one.
    private var currentPath: String?
    /// The stream playing, if it is one.
    private var currentURL: URL?
    /// Where the playlist came from: a URL's text or a path.
    private var playlistSource: String?
    private var playlistEntries: [WorkspaceVideoPlaylistEntry] = []
    /// The entry playing, or -1.
    private var playlistIndex = -1
    private var statusMessage: String?

    private var player: AVPlayer?
    private var itemStatusObservation: NSKeyValueObservation?
    private var timeControlObservation: NSKeyValueObservation?
    private var itemFailureObserver: NSObjectProtocol?
    private var interruptionObserver: NSObjectProtocol?
    private var holdsAudioSession = false
    private var pendingSessionRelease: DispatchWorkItem?
    private var wasPlayingBeforeInterruption = false
    /// Discards a stale load.
    private var loadGeneration = 0
    private var playlistRead: WorkspaceFileOperation?
    private var playlistFetcher: WorkspaceVideoPlaylistFetcher?
    private weak var ownPresentation: UIViewController?

    deinit {
        playlistRead?.cancel()
        playlistFetcher?.cancel()
        pendingSessionRelease?.cancel()
        if holdsAudioSession {
            WorkspaceVideoAudioSession.release()
        }
        if let itemFailureObserver {
            NotificationCenter.default.removeObserver(itemFailureObserver)
        }
        if let interruptionObserver {
            NotificationCenter.default.removeObserver(interruptionObserver)
        }
    }

    // MARK: - Testing

    var currentPathForTesting: String? { currentPath }
    var currentURLForTesting: String? { currentURL?.absoluteString }
    var statusMessageForTesting: String? { statusMessage }
    var isPlayingForTesting: Bool { player?.timeControlStatus == .playing }
    var hasPlayerForTesting: Bool { player != nil }
    var channelCountForTesting: Int { playlistEntries.count }
    var playlistIndexForTesting: Int { playlistIndex }
    var holdsAudioSessionForTesting: Bool { holdsAudioSession }

    // MARK: - Building

    override func viewDidLoad() {
        super.viewDidLoad()
        title = "Video"
        buildBar()
        buildPlayer()
        buildStatusLabel()
        interruptionObserver = NotificationCenter.default.addObserver(forName: AVAudioSession.interruptionNotification, object: nil, queue: .main) { [weak self] note in
            let type = (note.userInfo?[AVAudioSessionInterruptionTypeKey] as? UInt).flatMap(AVAudioSession.InterruptionType.init)
            let options = AVAudioSession.InterruptionOptions(rawValue: note.userInfo?[AVAudioSessionInterruptionOptionKey] as? UInt ?? 0)
            MainActor.assumeIsolated {
                self?.handleInterruption(type, options: options)
            }
        }
        setStatus("Open a video from the File Manager, or a stream with Open URL.")
    }

    /// Open URL, and Channels once a playlist with more than one entry is loaded. The window's
    /// own menu is Workspace's, so the player's actions live here, above the picture, where
    /// they stay reachable while it plays.
    private func buildBar() {
        configureBarButton(openURLButton, title: "Open URL", symbol: "link",
                           hint: "Plays a video stream or an .m3u playlist from a web address.",
                           identifier: "workspace.videoplayer.openURL", action: #selector(openURLTapped))
        configureBarButton(channelsButton, title: "Channels", symbol: "list.bullet",
                           hint: "Lists the entries of the loaded playlist.",
                           identifier: "workspace.videoplayer.channels", action: #selector(channelsTapped))
        channelsButton.isHidden = true
        bar.translatesAutoresizingMaskIntoConstraints = false
        bar.axis = .horizontal
        bar.spacing = 16
        [openURLButton, channelsButton, UIView()].forEach(bar.addArrangedSubview)
        toolContentView.addSubview(bar)
        NSLayoutConstraint.activate([
            bar.topAnchor.constraint(equalTo: toolContentView.topAnchor, constant: 4),
            bar.leadingAnchor.constraint(equalTo: toolContentView.leadingAnchor, constant: 12),
            bar.trailingAnchor.constraint(equalTo: toolContentView.trailingAnchor, constant: -12),
            bar.heightAnchor.constraint(equalToConstant: 32),
        ])
    }

    private func configureBarButton(_ button: UIButton, title: String, symbol: String, hint: String, identifier: String, action: Selector) {
        button.setTitle(" " + title, for: .normal)
        button.setImage(UIImage(systemName: symbol), for: .normal)
        button.titleLabel?.font = .systemFont(ofSize: 14, weight: .semibold)
        button.addTarget(self, action: action, for: .touchUpInside)
        button.setContentHuggingPriority(.required, for: .horizontal)
        button.accessibilityLabel = title
        button.accessibilityHint = hint
        button.accessibilityIdentifier = identifier
    }

    private func buildPlayer() {
        addChild(playerViewController)
        let playerView: UIView = playerViewController.view
        playerView.translatesAutoresizingMaskIntoConstraints = false
        // Shown once there's something to play.
        playerView.isHidden = true
        playerView.accessibilityIdentifier = "workspace.videoplayer.player"
        toolContentView.addSubview(playerView)
        NSLayoutConstraint.activate([
            playerView.topAnchor.constraint(equalTo: bar.bottomAnchor, constant: 4),
            playerView.leadingAnchor.constraint(equalTo: toolContentView.leadingAnchor),
            playerView.trailingAnchor.constraint(equalTo: toolContentView.trailingAnchor),
            playerView.bottomAnchor.constraint(equalTo: toolContentView.bottomAnchor),
        ])
        playerViewController.didMove(toParent: self)
    }

    private func buildStatusLabel() {
        statusLabel.translatesAutoresizingMaskIntoConstraints = false
        statusLabel.numberOfLines = 0
        statusLabel.textAlignment = .center
        statusLabel.font = .systemFont(ofSize: 15)
        statusLabel.accessibilityIdentifier = "workspace.videoplayer.status"
        toolContentView.addSubview(statusLabel)
        NSLayoutConstraint.activate([
            statusLabel.centerXAnchor.constraint(equalTo: toolContentView.centerXAnchor),
            statusLabel.centerYAnchor.constraint(equalTo: toolContentView.centerYAnchor),
            statusLabel.leadingAnchor.constraint(greaterThanOrEqualTo: toolContentView.leadingAnchor, constant: 20),
            statusLabel.trailingAnchor.constraint(lessThanOrEqualTo: toolContentView.trailingAnchor, constant: -20),
        ])
    }

    // MARK: - Theme

    override func applyTheme() {
        super.applyTheme()
        let colors = theme
        // The theme's text colours are only guaranteed against its card surfaces. Once a video
        // plays, the player covers this.
        toolContentView.backgroundColor = colors.card.withAlphaComponent(0.95)
        statusLabel.textColor = colors.secondary
        openURLButton.tintColor = colors.accent
        channelsButton.tintColor = colors.accent
    }

    // MARK: - Workspace

    /// `ws open` passes a stream's address through here as well.
    func workspaceOpenFile(atPath path: String) {
        loadViewIfNeeded()
        if WorkspaceVideoPlaylist.isRemote(path) {
            openURLString(path)
        } else {
            loadPath(path, autoplay: true)
        }
    }

    /// Which file was open, so a restored Workspace doesn't come back with an empty player: the
    /// path travels, not the media. A playlist travels as its source and the entry playing, and
    /// a stream as its address. The position stays behind: jumping to a stale offset in a file
    /// that may have changed isn't what a player should do on opening.
    func workspaceToolStateForSaving() -> [String: Any]? {
        if let playlistSource {
            return ["playlist": playlistSource, "entry": playlistIndex]
        }
        if let currentURL {
            return ["url": currentURL.absoluteString]
        }
        return currentPath.map { ["path": $0] }
    }

    /// Comes back paused, and a playlist doesn't raise its channel list. Those are for when
    /// someone opens one, not for an app starting up, perhaps with music from elsewhere playing.
    func workspaceRestoreToolState(_ state: [String: Any]) {
        loadViewIfNeeded()
        if let playlist = state["playlist"] as? String, !playlist.isEmpty {
            loadPlaylist(from: playlist, entry: (state["entry"] as? NSNumber)?.intValue ?? -1, opening: false)
        } else if let address = state["url"] as? String, !address.isEmpty {
            openURLString(address, autoplay: false)
        } else if let path = state["path"] as? String, !path.isEmpty {
            // A file deleted since then shows the load error, honestly.
            loadPath(path, autoplay: false)
        }
    }

    /// Takes the keyboard from whatever terminal had it, and finds out if the file has gone
    /// while this window was behind another. AVFoundation may hold it open and go on playing
    /// what's no longer anywhere.
    func workspaceToolDidBecomeFrontmost() {
        becomeFirstResponder()
        guard let path = currentPath, player != nil else { return }
        let generation = loadGeneration
        WorkspaceFileService.shared.item(path) { [weak self] result in
            guard let self, self.loadGeneration == generation else { return }
            switch result {
            case .success(let item) where item.kind == .regular:
                break
            case .success:
                self.resetToStatus(self.message(for: WorkspaceFileError.system(ENOENT), path: path))
            case .failure(let error):
                self.resetToStatus(self.message(for: error, path: path))
            }
        }
    }

    override var canBecomeFirstResponder: Bool { true }

    /// The window is closing: everything stops, and the session goes back.
    override func willMove(toParent parent: UIViewController?) {
        super.willMove(toParent: parent)
        if parent == nil {
            ownPresentation?.dismiss(animated: false)
            resetToStatus("")
        }
    }

    /// `presentedViewController` also answers for what an ancestor shows, such as another
    /// window's alert, so closing this window remembers which one is its own.
    private func presentOwned(_ controller: UIViewController) {
        ownPresentation = controller
        present(controller, animated: true)
    }

    private var isOnScreen: Bool {
        workspaceHost?.window(holding: self)?.isHidden == false
    }

    // MARK: - Status

    private func setStatus(_ text: String?) {
        statusMessage = text
        statusLabel.text = text
        statusLabel.isHidden = text == nil
    }

    private func message(for error: Error, path: String) -> String {
        if let fileError = error as? WorkspaceFileError {
            switch fileError {
            case .cancelled:
                return "Cancelled."
            case .tooLarge:
                return "This playlist is larger than 64 MB and can’t be read here."
            case .system(ENOENT):
                return "“\((path as NSString).lastPathComponent)” isn’t there any more. It may have been deleted or moved."
            default:
                break
            }
        }
        return error.localizedDescription.isEmpty ? "Couldn’t open this video." : error.localizedDescription
    }

    /// AVFoundation says both what happened and why: "Cannot Open" and "This media may be
    /// damaged". Nothing here keeps a list of what it can play: anything goes to it, and a
    /// container it can't read fails with its own accurate word.
    private func message(forPlaybackError error: Error?) -> String {
        guard let error = error as NSError? else { return "This video or stream can’t be played." }
        let parts = [error.localizedDescription, error.localizedFailureReason].compactMap { $0 }.filter { !$0.isEmpty }
        return parts.isEmpty ? "This video or stream can’t be played." : parts.joined(separator: ". ")
    }

    // MARK: - Loading

    private func cancelLoads() {
        playlistRead?.cancel()
        playlistRead = nil
        playlistFetcher?.cancel()
        playlistFetcher = nil
    }

    private func stopPlayer() {
        itemStatusObservation = nil
        timeControlObservation = nil
        if let itemFailureObserver {
            NotificationCenter.default.removeObserver(itemFailureObserver)
            self.itemFailureObserver = nil
        }
        playerViewController.player = nil
        player?.pause()
        player?.replaceCurrentItem(with: nil)
        player = nil
        wasPlayingBeforeInterruption = false
        setHoldsAudioSession(false)
    }

    /// Everything playing or loading stops, and the status line says `text`.
    private func resetToStatus(_ text: String) {
        cancelLoads()
        stopPlayer()
        loadGeneration += 1
        playerViewController.view.isHidden = true
        setStatus(text)
    }

    private func forgetPlaylist() {
        playlistSource = nil
        playlistEntries = []
        playlistIndex = -1
        channelsButton.isHidden = true
    }

    /// A file: a playlist is read and listed, and anything else played.
    private func loadPath(_ path: String, autoplay: Bool) {
        if WorkspaceVideoPlaylist.isPlaylistPath(path) {
            loadPlaylist(from: path, entry: -1, opening: autoplay)
            return
        }
        forgetPlaylist()
        loadMedia(atPath: path, autoplay: autoplay)
    }

    private func loadMedia(atPath path: String, autoplay: Bool) {
        resetToStatus("Loading…")
        currentPath = path
        currentURL = nil
        let generation = loadGeneration
        WorkspaceFileService.shared.item(path) { [weak self] result in
            guard let self, self.loadGeneration == generation else { return }
            switch result {
            case .success(let item) where item.kind == .regular && item.size == 0:
                self.resetToStatus("This file is empty.")
            case .success(let item) where item.kind == .regular:
                self.startPlaying(path: path, autoplay: autoplay, generation: generation)
            case .success(let item):
                let error: WorkspaceFileError = item.isBrokenSymlink ? .system(ENOENT) : .notRegularFile
                self.resetToStatus(self.message(for: error, path: path))
            case .failure(let error):
                self.resetToStatus(self.message(for: error, path: path))
            }
        }
    }

    private func startPlaying(path: String, autoplay: Bool, generation: Int) {
        Self.fileQueue.async { [weak self] in
            let url = WorkspaceFileService.fileURL(path)
            DispatchQueue.main.async { [weak self] in
                MainActor.assumeIsolated {
                    guard let self, self.loadGeneration == generation else { return }
                    self.play(url, autoplay: autoplay)
                }
            }
        }
    }

    private func play(_ url: URL, autoplay: Bool) {
        stopPlayer()
        let item = AVPlayerItem(url: url)
        itemStatusObservation = item.observe(\.status) { [weak self] item, _ in
            DispatchQueue.main.async { [weak self] in
                MainActor.assumeIsolated {
                    guard let self, self.player?.currentItem === item, item.status == .failed else { return }
                    self.resetToStatus(self.message(forPlaybackError: item.error))
                }
            }
        }
        // A file that goes bad or goes away partway through, or a stream that drops.
        itemFailureObserver = NotificationCenter.default.addObserver(forName: .AVPlayerItemFailedToPlayToEndTime, object: item, queue: .main) { [weak self] note in
            let error = note.userInfo?[AVPlayerItemFailedToPlayToEndTimeErrorKey] as? NSError
            MainActor.assumeIsolated {
                guard let self else { return }
                self.resetToStatus(self.message(forPlaybackError: error))
            }
        }
        let player = AVPlayer(playerItem: item)
        timeControlObservation = player.observe(\.timeControlStatus) { [weak self] _, _ in
            DispatchQueue.main.async { [weak self] in
                MainActor.assumeIsolated {
                    self?.playbackStateChanged()
                }
            }
        }
        self.player = player
        playerViewController.player = player
        playerViewController.view.isHidden = false
        setStatus(nil)
        if autoplay {
            setHoldsAudioSession(true)
            player.play()
        }
    }

    // MARK: - Audio session

    private func setHoldsAudioSession(_ holds: Bool) {
        pendingSessionRelease?.cancel()
        pendingSessionRelease = nil
        guard holds != holdsAudioSession else { return }
        holdsAudioSession = holds
        if holds {
            // Within one app no interruption does this for us, and the two would play at once.
            WorkspaceAudioEngine.shared.pause()
            WorkspaceVideoAudioSession.acquire()
        } else {
            WorkspaceVideoAudioSession.release()
        }
    }

    /// The session is held from the moment the player means to play, buffering included, until
    /// it has been paused for a little while. The player's own controls can change this at any
    /// time, so it follows them.
    private func playbackStateChanged() {
        guard let player else { return }
        if player.timeControlStatus != .paused {
            setHoldsAudioSession(true)
        } else if holdsAudioSession, pendingSessionRelease == nil {
            let release = DispatchWorkItem { [weak self] in
                MainActor.assumeIsolated {
                    self?.setHoldsAudioSession(false)
                }
            }
            pendingSessionRelease = release
            DispatchQueue.main.asyncAfter(deadline: .now() + Self.sessionReleaseDelay, execute: release)
        }
    }

    /// A call or another app took the audio, and the system paused the player. It goes on when
    /// the system says it may, if it was playing.
    private func handleInterruption(_ type: AVAudioSession.InterruptionType?, options: AVAudioSession.InterruptionOptions) {
        guard let player else { return }
        switch type {
        case .began:
            wasPlayingBeforeInterruption = player.timeControlStatus != .paused || holdsAudioSession
        case .ended:
            if options.contains(.shouldResume), wasPlayingBeforeInterruption {
                setHoldsAudioSession(true)
                player.play()
            }
            wasPlayingBeforeInterruption = false
        default:
            break
        }
    }

    // MARK: - Streams and playlists

    @objc private func openURLTapped() {
        let alert = UIAlertController(title: "Open URL", message: "A video, an HLS stream (.m3u8), or an .m3u playlist.", preferredStyle: .alert)
        // Only a link on the clipboard: reading text that isn't one would ask the person's
        // leave to paste every time this opens, and fill the field with nothing useful.
        let clip = UIPasteboard.general.hasURLs ? UIPasteboard.general.url?.absoluteString : nil
        alert.addTextField { field in
            field.placeholder = "https://"
            field.keyboardType = .URL
            field.autocapitalizationType = .none
            field.autocorrectionType = .no
            field.clearButtonMode = .whileEditing
            field.accessibilityIdentifier = "workspace.videoplayer.urlField"
            if let clip, WorkspaceVideoPlaylist.isRemote(clip) {
                field.text = clip
            }
        }
        alert.addAction(UIAlertAction(title: "Cancel", style: .cancel))
        alert.addAction(UIAlertAction(title: "Open", style: .default) { [weak self, weak alert] _ in
            let text = alert?.textFields?.first?.text?.trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
            if !text.isEmpty {
                self?.openURLString(text)
            }
        })
        presentOwned(alert)
    }

    private func openURLString(_ text: String, autoplay: Bool = true) {
        guard WorkspaceVideoPlaylist.isRemote(text), let url = WorkspaceVideoPlaylist.webURL(text) else {
            forgetPlaylist()
            resetToStatus("That is not an http:// or https:// address.")
            return
        }
        if WorkspaceVideoPlaylist.looksLikePlaylist(url) {
            loadPlaylist(from: url.absoluteString, entry: -1, opening: autoplay)
            return
        }
        forgetPlaylist()
        playRemoteURL(url, autoplay: autoplay)
    }

    private func playRemoteURL(_ url: URL, autoplay: Bool) {
        resetToStatus("Loading…")
        currentPath = nil
        currentURL = url
        play(url, autoplay: autoplay)
    }

    /// Reads a playlist, from an address or a file, and acts on what it is. `entry`, if it's in
    /// the list, starts paused: only a restore passes one. `opening` says someone opened this
    /// just now, so it may play on its own and raise the channel list.
    private func loadPlaylist(from source: String, entry: Int, opening: Bool) {
        forgetPlaylist()
        resetToStatus("Reading playlist…")
        currentPath = nil
        currentURL = nil
        // From now, so a saved layout still knows it if the read fails.
        playlistSource = source
        let generation = loadGeneration

        if WorkspaceVideoPlaylist.isRemote(source) {
            guard let url = WorkspaceVideoPlaylist.webURL(source) else {
                resetToStatus("This playlist’s address is not valid.")
                return
            }
            let fetcher = WorkspaceVideoPlaylistFetcher(url: url) { [weak self] outcome in
                guard let self, self.loadGeneration == generation else { return }
                self.playlistFetcher = nil
                switch outcome {
                case .data(let data):
                    self.parsePlaylist(data, from: source, entry: entry, opening: opening, generation: generation)
                case .notAPlaylist:
                    // An IPTV link can answer with the stream itself.
                    self.forgetPlaylist()
                    self.playRemoteURL(url, autoplay: opening)
                case .failure(let message):
                    self.resetToStatus(message)
                }
            }
            playlistFetcher = fetcher
            fetcher.start()
            return
        }

        playlistRead = WorkspaceFileService.shared.readFile(source, maxBytes: WorkspaceVideoPlaylist.maxBytes) { [weak self] result in
            guard let self, self.loadGeneration == generation else { return }
            self.playlistRead = nil
            switch result {
            case .success(let data):
                self.parsePlaylist(data, from: source, entry: entry, opening: opening, generation: generation)
            case .failure(let error):
                self.resetToStatus(self.message(for: error, path: source))
            }
        }
    }

    /// Off the main thread, since a list can hold hundreds of thousands of channels.
    private func parsePlaylist(_ data: Data, from source: String, entry: Int, opening: Bool, generation: Int) {
        Self.fileQueue.async { [weak self] in
            let text = WorkspaceVideoPlaylist.decode(data)
            let parsed = text.map(WorkspaceVideoPlaylist.parse)
            let entries = parsed.map { WorkspaceVideoPlaylist.resolved($0.entries, source: source) } ?? []
            let isHLS = parsed?.isHLS ?? false
            let isText = text != nil
            DispatchQueue.main.async { [weak self] in
                MainActor.assumeIsolated {
                    guard let self, self.loadGeneration == generation else { return }
                    self.handlePlaylist(entries: entries, isHLS: isHLS, isText: isText, source: source, entry: entry, opening: opening)
                }
            }
        }
    }

    private func handlePlaylist(entries: [WorkspaceVideoPlaylistEntry], isHLS: Bool, isText: Bool, source: String, entry: Int, opening: Bool) {
        let remoteURL = WorkspaceVideoPlaylist.isRemote(source) ? WorkspaceVideoPlaylist.webURL(source) : nil
        if isHLS {
            // One stream, which AVPlayer reads from its URL, fetching the segments it names
            // relative to that. A file on its own has no URL they can be fetched against.
            forgetPlaylist()
            if let remoteURL {
                playRemoteURL(remoteURL, autoplay: opening)
            } else {
                resetToStatus("This is an HLS stream. Open its web address with Open URL.")
            }
            return
        }
        if entries.isEmpty {
            // Not a list after all: an IPTV link can answer with the stream itself.
            if let remoteURL {
                forgetPlaylist()
                playRemoteURL(remoteURL, autoplay: opening)
            } else {
                resetToStatus(isText ? "This playlist lists nothing to play." : "This doesn’t look like a playlist.")
            }
            return
        }
        playlistEntries = entries
        channelsButton.isHidden = entries.count < 2
        if entries.indices.contains(entry) {
            playEntry(entry, autoplay: false)
        } else if entries.count == 1 {
            playEntry(0, autoplay: opening)
        } else {
            setStatus("\(entries.count) entries. Choose one with Channels.")
            // Not over whatever is on another Desktop now, if the read took a while.
            if opening && isOnScreen {
                channelsTapped()
            }
        }
    }

    private func playEntry(_ index: Int, autoplay: Bool) {
        guard playlistEntries.indices.contains(index) else { return }
        playlistIndex = index
        let location = playlistEntries[index].location
        if WorkspaceVideoPlaylist.isRemote(location) {
            if let url = WorkspaceVideoPlaylist.webURL(location) {
                playRemoteURL(url, autoplay: autoplay)
            } else {
                resetToStatus("This entry’s address is not valid.")
            }
        } else if WorkspaceVideoPlaylist.hasOtherScheme(location) || playlistSource.map(WorkspaceVideoPlaylist.isRemote) == true {
            // A list from the web names what's on the web, never a file here.
            resetToStatus("This entry’s address can’t be played here: only http and https.")
        } else {
            loadMedia(atPath: location, autoplay: autoplay)
        }
    }

    @objc private func channelsTapped() {
        guard !playlistEntries.isEmpty, presentedViewController == nil else { return }
        let picker = WorkspaceVideoPlaylistPicker(entries: playlistEntries) { [weak self] index in
            self?.playEntry(index, autoplay: true)
        }
        let navigation = UINavigationController(rootViewController: picker)
        navigation.modalPresentationStyle = .formSheet
        presentOwned(navigation)
    }
}
