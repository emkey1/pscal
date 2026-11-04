// Copyright 2026 Mike Miller
// SPDX-License-Identifier: Apache-2.0
//
// Ported by its author from iSH-AOK's Workspace (app/AudioPlayerEngine.m at 0f75fee1c) and
// licensed to PSCAL under the Apache License 2.0.

import AVFoundation
import MediaPlayer
import QuartzCore

// The app's one music player. The Music window is a remote control for it, so closing that
// window doesn't stop playback, and the Equalizer acts on it whether or not Music is open.
//
// Two halves. `WorkspaceAudioEngine` keeps the queue and the transport state on the main
// thread, and never touches AVFoundation. `WorkspaceAudioPipeline` does nothing else, all of it
// on one serial queue, which rules out the crash AVFoundation offers to a player that schedules
// a buffer while another thread reconnects it to a different format, and keeps the blocking
// session calls away from the interface.

extension Notification.Name {
    /// All posted on the main thread, with nothing attached: read the engine's properties.
    /// `currentTime` isn't notified, since it changes continuously; poll it while visible.
    static let workspaceAudioStateDidChange = Notification.Name("WorkspaceAudioStateDidChange")
    static let workspaceAudioTrackDidChange = Notification.Name("WorkspaceAudioTrackDidChange")
    static let workspaceAudioQueueDidChange = Notification.Name("WorkspaceAudioQueueDidChange")
    static let workspaceEqualizerDidChange = Notification.Name("WorkspaceEqualizerDidChange")
}

enum WorkspaceAudioPlaybackState {
    case stopped, loading, playing, paused
}

enum WorkspaceAudioRepeatMode: Int {
    /// Stop after the last track, wrap to the first, or loop the current one.
    case off = 0, all, one
}

/// One queue entry, identified by its path as the shell sees it. Tags are filled in when the
/// track first loads.
struct WorkspaceAudioTrack: Sendable {
    let path: String
    var title: String
    var artist: String?
    var album: String?
    var duration: TimeInterval = 0

    init(path: String) {
        self.path = path
        let name = (path as NSString).lastPathComponent
        let stem = (name as NSString).deletingPathExtension
        title = stem.isEmpty ? name : stem
    }

    /// The form a playlist's JSON holds.
    var dictionaryRepresentation: [String: Any] {
        var dictionary: [String: Any] = ["path": path, "title": title]
        dictionary["artist"] = artist
        dictionary["album"] = album
        if duration > 0 {
            dictionary["duration"] = duration
        }
        return dictionary
    }

    init?(dictionary: [String: Any]) {
        guard let path = dictionary["path"] as? String, !path.isEmpty else { return nil }
        self.init(path: path)
        if let title = dictionary["title"] as? String, !title.isEmpty {
            self.title = title
        }
        artist = dictionary["artist"] as? String
        album = dictionary["album"] as? String
        duration = dictionary["duration"] as? TimeInterval ?? 0
    }
}

#if targetEnvironment(macCatalyst)
// The Mac build has no SDL.
private func workspaceSDLAudioIsOpen() -> Bool {
    false
}
#else
@_silgen_name("Mix_QuerySpec")
private func mixQuerySpecC(_ frequency: UnsafeMutablePointer<Int32>?, _ format: UnsafeMutablePointer<UInt16>?, _ channels: UnsafeMutablePointer<Int32>?) -> Int32

/// Whether an SDL program in a terminal has its sound open (`InitSoundSystem`). Any thread.
private func workspaceSDLAudioIsOpen() -> Bool {
    mixQuerySpecC(nil, nil, nil) > 0
}
#endif

/// Who's on the app's one audio session. Deactivating a session stops all the audio on it,
/// whoever started it, and only says so afterwards, so Music leaves the session alone while
/// anyone else plays. SDL programs are found by asking SDL_mixer. Other playback in the app, such
/// as a video, says so with `begin()` when it starts and `end()` when it stops, and asks
/// `musicOrSDLIsPlaying` before changing the session itself. Any thread.
enum WorkspaceAudioSessionSharing {
    private static let lock = NSLock()
    private nonisolated(unsafe) static var users = 0
    private nonisolated(unsafe) static var musicHolds = false

    static func begin() {
        lock.lock()
        users += 1
        lock.unlock()
    }

    static func end() {
        lock.lock()
        users = max(0, users - 1)
        lock.unlock()
    }

    static var musicOrSDLIsPlaying: Bool {
        musicHoldsSession || workspaceSDLAudioIsOpen()
    }

    fileprivate static var musicHoldsSession: Bool {
        get {
            lock.lock()
            defer { lock.unlock() }
            return musicHolds
        }
        set {
            lock.lock()
            musicHolds = newValue
            lock.unlock()
        }
    }

    /// Someone other than Music.
    fileprivate static var isSharedWithOthers: Bool {
        lock.lock()
        let shared = users > 0
        lock.unlock()
        return shared || workspaceSDLAudioIsOpen()
    }
}

private struct WorkspaceAudioPosition {
    var seconds: TimeInterval = 0
    var stamp: CFTimeInterval = 0
    var advancing = false

    var now: TimeInterval {
        advancing ? seconds + max(0, CACurrentMediaTime() - stamp) : seconds
    }
}

private struct WorkspaceAudioLoaded: Sendable {
    let title: String?
    let artist: String?
    let album: String?
    let duration: TimeInterval
}

private enum WorkspaceAudioLoadOutcome: Sendable {
    case loaded(WorkspaceAudioLoaded)
    case failed(String)
}

private enum WorkspaceAudioPipelineEvent: Sendable {
    /// The last of the track has played. `readFailed` if it ended early on a read error.
    case ended(readFailed: Bool)
    /// The output stopped under it: a configuration change, or the session taken away.
    case outputLost
    /// The file was deleted or moved.
    case fileGone
}

private func workspaceAudioMetadata(url: URL) -> (title: String?, artist: String?, album: String?) {
    let items = AVURLAsset(url: url).commonMetadata
    func value(_ key: AVMetadataKey) -> String? {
        items.first { $0.commonKey == key }?.stringValue
    }
    return (value(.commonKeyTitle), value(.commonKeyArtist), value(.commonKeyAlbumName))
}

/// Everything that touches AVFoundation, on one serial queue. Only the published position is
/// read from elsewhere.
///
/// A file is opened and read a chunk at a time, so at most a few seconds of it are ever in
/// memory. A "pass" is one stretch of playing from one position: starting another cancels the
/// last, whose buffers are then flushed and whose callbacks do nothing.
private final class WorkspaceAudioPipeline: @unchecked Sendable {
    // Larger chunks and a deep cushion keep playback gapless when a busy shell has the CPU: few
    // scheduling events, and seconds of audio ahead of the play head to ride out a stall.
    private static let chunkFrames: AVAudioFrameCount = 16384
    private static let inFlightSeconds = 6.0
    private static let monitorInterval: TimeInterval = 0.5
    /// Render time standing still this many ticks means the output is gone. A session taken away
    /// by someone else's deactivation shows up no other way.
    private static let stalledTicksLimit = 3
    /// eofErr. A read starting past the end throws it, and a length can be only an estimate.
    private static let endOfFileError = -39

    // Above where shells run, so refills get the core when one is pegged. The bursts are tiny.
    private let queue = DispatchQueue(label: "com.pscal.workspace.audio", qos: .userInteractive)

    /// Set once, before any work starts.
    var eventHandler: (@MainActor (WorkspaceAudioPipelineEvent, WorkspaceFileOperation) -> Void)?

    private let lock = NSLock()
    private var positionValue = WorkspaceAudioPosition()

    // The queue's alone from here on.
    private var engine: AVAudioEngine?
    private var player: AVAudioPlayerNode?
    private var equalizer: AVAudioUnitEQ?
    private var configurationObserver: NSObjectProtocol?
    private var connectedFormat: AVAudioFormat?
    private var graphIsStale = false
    private var currentPass: WorkspaceFileOperation?
    private var file: AVAudioFile?
    private var hostPath: String?
    private var inFlightFrames: AVAudioFramePosition = 0
    private var fileAtEnd = false
    private var readFailed = false
    private var baseTime: TimeInterval = 0
    private var monitor: DispatchSourceTimer?
    private var lastRenderSample: AVAudioFramePosition?
    private var stalledTicks = 0
    private var volume: Float = 1
    private var equalizerEnabled = false
    private var equalizerGains: [Float] = []

    // MARK: - Any thread

    var position: TimeInterval {
        lock.lock()
        defer { lock.unlock() }
        return positionValue.now
    }

    func setPosition(_ seconds: TimeInterval, advancing: Bool = false) {
        lock.lock()
        positionValue = WorkspaceAudioPosition(seconds: seconds, stamp: CACurrentMediaTime(), advancing: advancing)
        lock.unlock()
    }

    private func onMain(_ work: @escaping @MainActor () -> Void) {
        DispatchQueue.main.async {
            MainActor.assumeIsolated(work)
        }
    }

    private func deliver(_ event: WorkspaceAudioPipelineEvent, pass: WorkspaceFileOperation) {
        let handler = eventHandler
        onMain {
            handler?(event, pass)
        }
    }

    // MARK: - Settings

    func configure(volume: Float, equalizerEnabled: Bool, equalizerGains: [Float]) {
        queue.async { [self] in
            self.volume = volume
            self.equalizerEnabled = equalizerEnabled
            self.equalizerGains = equalizerGains
        }
    }

    func setVolume(_ volume: Float) {
        queue.async { [self] in
            self.volume = volume
            engine?.mainMixerNode.outputVolume = volume
        }
    }

    func setEqualizer(enabled: Bool, gains: [Float]) {
        queue.async { [self] in
            equalizerEnabled = enabled
            equalizerGains = gains
            applyEqualizer()
        }
    }

    private func applyEqualizer() {
        guard let equalizer else { return }
        for (index, band) in equalizer.bands.enumerated() where index < equalizerGains.count {
            band.gain = equalizerGains[index]
        }
        equalizer.bypass = !equalizerEnabled
    }

    // MARK: - The graph

    /// A new engine each time. It's the one restart that leaves nothing stale behind, whatever
    /// a configuration change or a reset of the media server did to the old one.
    private func buildGraph() {
        if let configurationObserver {
            NotificationCenter.default.removeObserver(configurationObserver)
        }
        self.engine?.stop()
        let engine = AVAudioEngine()
        let player = AVAudioPlayerNode()
        let frequencies = WorkspaceAudioEqualizer.frequencies
        let equalizer = AVAudioUnitEQ(numberOfBands: frequencies.count)
        for (index, frequency) in frequencies.enumerated() {
            let band = equalizer.bands[index]
            band.filterType = index == 0 ? .lowShelf : (index + 1 == frequencies.count ? .highShelf : .parametric)
            band.frequency = frequency
            band.bandwidth = 1
            band.bypass = false
        }
        engine.attach(player)
        engine.attach(equalizer)
        engine.mainMixerNode.outputVolume = volume
        self.engine = engine
        self.player = player
        self.equalizer = equalizer
        connectedFormat = nil
        graphIsStale = false
        applyEqualizer()
        // On one of the engine's own threads, where deleting it could deadlock, so hop first.
        configurationObserver = NotificationCenter.default.addObserver(forName: .AVAudioEngineConfigurationChange, object: engine, queue: nil) { [weak self, weak engine] _ in
            guard let self, let engine else { return }
            self.queue.async {
                self.engineConfigurationChanged(engine)
            }
        }
    }

    /// The player to the equalizer to the mixer, at this track's own format; the mixer resamples
    /// for the output. Only a stopped engine is reconnected, and only when the format differs.
    private func connect(_ format: AVAudioFormat) {
        guard let engine, let player, let equalizer else { return }
        if let connectedFormat, connectedFormat.isEqual(format) {
            return
        }
        if engine.isRunning {
            engine.stop()
        }
        engine.connect(player, to: equalizer, format: format)
        engine.connect(equalizer, to: engine.mainMixerNode, format: format)
        connectedFormat = format
    }

    /// The engine has already stopped itself. If nothing's playing, the next start rebuilds.
    private func engineConfigurationChanged(_ changed: AVAudioEngine) {
        guard changed === engine else { return }
        graphIsStale = true
        if let currentPass {
            stopMonitor()
            deliver(.outputLost, pass: currentPass)
        }
    }

    /// After a reset of the media server, when every audio object is dead.
    func discardGraph() {
        queue.async { [self] in
            if let configurationObserver {
                NotificationCenter.default.removeObserver(configurationObserver)
                self.configurationObserver = nil
            }
            stopMonitor()
            engine = nil
            player = nil
            equalizer = nil
            connectedFormat = nil
            graphIsStale = true
            WorkspaceAudioSessionSharing.musicHoldsSession = false
        }
    }

    // MARK: - The session

    /// Set just before each activation, because an SDL program sets its own category and it
    /// outlives that program. While someone else is playing their category stays, since changing
    /// it would disturb them, and Music mixes in with what they chose.
    private func activateSession() throws {
        let session = AVAudioSession.sharedInstance()
        if !WorkspaceAudioSessionSharing.isSharedWithOthers {
            if session.category != .playback || session.mode != .default || !session.categoryOptions.isEmpty {
                try session.setCategory(.playback, mode: .default, options: [])
            }
            // Playback needs no low latency, and more slack means fewer underruns.
            try? session.setPreferredIOBufferDuration(0.046)
        }
        try session.setActive(true)
        WorkspaceAudioSessionSharing.musicHoldsSession = true
    }

    private func releaseSession() {
        guard WorkspaceAudioSessionSharing.musicHoldsSession else { return }
        WorkspaceAudioSessionSharing.musicHoldsSession = false
        guard !WorkspaceAudioSessionSharing.isSharedWithOthers else { return }
        try? AVAudioSession.sharedInstance().setActive(false, options: .notifyOthersOnDeactivation)
    }

    // MARK: - Loading

    /// Opens a file and schedules its first seconds, ready for `startOutput`.
    func load(path: String, from start: TimeInterval, pass: WorkspaceFileOperation, completion: @escaping @MainActor (WorkspaceAudioLoadOutcome) -> Void) {
        setPosition(start)
        queue.async { [self] in
            guard !pass.isCancelled else { return }
            stopMonitor()
            currentPass = pass
            player?.stop()
            file = nil
            hostPath = nil
            let outcome = open(path: path, from: start, pass: pass)
            onMain {
                completion(outcome)
            }
        }
    }

    private func open(path: String, from start: TimeInterval, pass: WorkspaceFileOperation) -> WorkspaceAudioLoadOutcome {
        let name = (path as NSString).lastPathComponent
        func refusal(_ reason: String) -> WorkspaceAudioLoadOutcome {
            .failed("Can’t play “\(name)”: \(reason)")
        }
        let host = WorkspaceFileService.hostPath(path)
        var info = Darwin.stat()
        guard stat(host, &info) == 0 else {
            let code = errno
            return refusal(code == ENOENT ? "it isn’t there any more." : String(cString: strerror(code)))
        }
        // AVFoundation would block for good on a FIFO.
        guard info.st_mode & S_IFMT == S_IFREG else { return refusal("it isn’t a regular file.") }
        guard info.st_size > 0 else { return refusal("it’s empty.") }

        let url = URL(fileURLWithPath: host)
        let opened: AVAudioFile
        do {
            opened = try AVAudioFile(forReading: url, commonFormat: .pcmFormatFloat32, interleaved: false)
        } catch {
            return refusal("this device can’t read it. It may be damaged, or in a format iOS doesn’t decode.")
        }
        // Connecting a format with no rate or channels raises an exception nothing can catch.
        let format = opened.processingFormat
        guard format.sampleRate > 0, format.channelCount > 0, opened.length > 0 else {
            return refusal("it holds no audio.")
        }
        // Without a layout the mixer doesn't know where they go, and plays silence.
        if format.channelCount > 2, let tag = format.channelLayout?.layoutTag, tag & 0xFFFF_0000 == kAudioChannelLayoutTag_DiscreteInOrder {
            return refusal("its \(format.channelCount) channels have no layout to mix down.")
        }
        let metadata = workspaceAudioMetadata(url: url)
        guard !pass.isCancelled else { return refusal("cancelled.") }

        let duration = Double(opened.length) / format.sampleRate
        if engine == nil || graphIsStale {
            buildGraph()
        }
        connect(format)
        file = opened
        hostPath = host
        let startAt = start < duration ? start : 0
        setPosition(startAt)
        prime(opened, at: startAt, pass: pass)
        // A header that parses over a body that doesn't.
        guard inFlightFrames > 0 else {
            file = nil
            hostPath = nil
            return refusal("this device can’t read it. It may be damaged, or in a format iOS doesn’t decode.")
        }
        return .loaded(WorkspaceAudioLoaded(title: metadata.title, artist: metadata.artist, album: metadata.album, duration: duration))
    }

    /// Flushes the player and schedules from `time`.
    private func prime(_ file: AVAudioFile, at time: TimeInterval, pass: WorkspaceFileOperation) {
        player?.stop()
        inFlightFrames = 0
        fileAtEnd = false
        readFailed = false
        baseTime = time
        let frame = AVAudioFramePosition((time * file.processingFormat.sampleRate).rounded())
        file.framePosition = max(0, min(frame, file.length))
        fill(pass: pass)
    }

    /// Tops up the player so that some seconds are always scheduled ahead of the play head.
    private func fill(pass: WorkspaceFileOperation) {
        guard pass === currentPass, let file, let player else { return }
        let format = file.processingFormat
        let target = AVAudioFramePosition(Self.inFlightSeconds * format.sampleRate)
        while !fileAtEnd && inFlightFrames < target {
            if pass.isCancelled {
                return
            }
            guard let buffer = AVAudioPCMBuffer(pcmFormat: format, frameCapacity: Self.chunkFrames) else {
                fileAtEnd = true
                break
            }
            do {
                try file.read(into: buffer, frameCount: Self.chunkFrames)
            } catch {
                fileAtEnd = true
                readFailed = (error as NSError).code != Self.endOfFileError
                break
            }
            let frames = buffer.frameLength
            if frames == 0 {
                fileAtEnd = true
                break
            }
            if file.framePosition >= file.length {
                fileAtEnd = true
            }
            inFlightFrames += AVAudioFramePosition(frames)
            // Fires on an internal thread when the player has used the buffer, or at once on a
            // stop, so it does nothing more than get back to this queue.
            player.scheduleBuffer(buffer, at: nil, options: [], completionCallbackType: .dataRendered) { [self] _ in
                queue.async { [self] in
                    bufferFinished(frames: frames, pass: pass)
                }
            }
        }
    }

    private func bufferFinished(frames: AVAudioFrameCount, pass: WorkspaceFileOperation) {
        guard pass === currentPass, !pass.isCancelled else { return }
        inFlightFrames -= AVAudioFramePosition(frames)
        if fileAtEnd && inFlightFrames <= 0 {
            reportEnded(pass: pass)
        } else {
            fill(pass: pass)
        }
    }

    /// Rendered isn't heard yet, and moving on at once would cut off the very end.
    private func reportEnded(pass: WorkspaceFileOperation) {
        let tail = max(0, engine?.outputNode.presentationLatency ?? 0) + 0.1
        queue.asyncAfter(deadline: .now() + tail) { [self] in
            guard pass === currentPass, !pass.isCancelled else { return }
            stopMonitor()
            deliver(.ended(readFailed: readFailed), pass: pass)
        }
    }

    // MARK: - Output

    /// Starts sound for what's loaded. `request` is cancelled if a pause overtakes it.
    func startOutput(pass: WorkspaceFileOperation, request: WorkspaceFileOperation, completion: @escaping @MainActor (String?) -> Void) {
        queue.async { [self] in
            guard !request.isCancelled, pass === currentPass else { return }
            let failure = beginOutput(pass: pass, request: request)
            onMain {
                completion(failure)
            }
        }
    }

    private func beginOutput(pass: WorkspaceFileOperation, request: WorkspaceFileOperation) -> String? {
        if graphIsStale, let file {
            let resumeAt = position
            buildGraph()
            connect(file.processingFormat)
            prime(file, at: resumeAt, pass: pass)
        }
        guard let engine, let player else { return "Nothing is loaded to play." }
        do {
            try activateSession()
            if !engine.isRunning {
                try engine.start()
            }
        } catch {
            releaseSession()
            return "Audio output isn’t available: \(error.localizedDescription)"
        }
        // Paused while the session was activating. That pause is queued behind this.
        guard !request.isCancelled, pass === currentPass else { return nil }
        // Playing a node on a stopped engine raises an exception nothing here could catch.
        guard engine.isRunning else {
            releaseSession()
            return "Audio output isn’t available."
        }
        player.play()
        setPosition(position, advancing: true)
        startMonitor(pass: pass)
        return nil
    }

    func pauseOutput() {
        setPosition(position)
        queue.async { [self] in
            stopMonitor()
            if let played = playedSeconds() {
                setPosition(played)
            }
            player?.pause()
            if let engine, engine.isRunning {
                engine.pause()
            }
            releaseSession()
        }
    }

    func stopOutput(pass: WorkspaceFileOperation) {
        setPosition(0)
        queue.async { [self] in
            stopMonitor()
            currentPass = pass
            player?.stop()
            file = nil
            hostPath = nil
            inFlightFrames = 0
            fileAtEnd = false
            engine?.stop()
            releaseSession()
        }
    }

    /// A seek, or with `rebuild` a clean restart of the output, at the same file.
    func reposition(to time: TimeInterval, play: Bool, rebuild: Bool, pass: WorkspaceFileOperation, request: WorkspaceFileOperation, completion: @escaping @MainActor (String?) -> Void) {
        setPosition(time)
        queue.async { [self] in
            guard !pass.isCancelled else { return }
            stopMonitor()
            currentPass = pass
            guard let file else { return }
            if rebuild || graphIsStale || engine == nil {
                buildGraph()
            }
            connect(file.processingFormat)
            prime(file, at: time, pass: pass)
            if fileAtEnd && inFlightFrames <= 0 {
                reportEnded(pass: pass)
                return
            }
            let failure = play ? beginOutput(pass: pass, request: request) : nil
            onMain {
                completion(failure)
            }
        }
    }

    // MARK: - The monitor

    private func playedSeconds() -> TimeInterval? {
        guard let player, let render = player.lastRenderTime, render.isSampleTimeValid,
              let played = player.playerTime(forNodeTime: render), played.isSampleTimeValid, played.sampleRate > 0 else { return nil }
        return baseTime + Double(played.sampleTime) / played.sampleRate
    }

    private func startMonitor(pass: WorkspaceFileOperation) {
        stopMonitor()
        lastRenderSample = nil
        stalledTicks = 0
        let timer = DispatchSource.makeTimerSource(queue: queue)
        timer.schedule(deadline: .now() + Self.monitorInterval, repeating: Self.monitorInterval)
        timer.setEventHandler { [weak self] in
            self?.monitorTick(pass: pass)
        }
        timer.resume()
        monitor = timer
    }

    private func stopMonitor() {
        monitor?.cancel()
        monitor = nil
    }

    /// While playing: publishes the position, notices an output that died without a word (an SDL
    /// program opening or closing its sound deactivates the session under this one), and notices
    /// a file that's gone. An open file stays readable after it's deleted, so nothing else would.
    private func monitorTick(pass: WorkspaceFileOperation) {
        guard pass === currentPass, !pass.isCancelled, let engine, let player else {
            stopMonitor()
            return
        }
        if let render = player.lastRenderTime, render.isSampleTimeValid {
            stalledTicks = render.sampleTime == lastRenderSample ? stalledTicks + 1 : 0
            lastRenderSample = render.sampleTime
        } else {
            stalledTicks += 1
        }
        if let played = playedSeconds() {
            setPosition(played, advancing: true)
        }
        if !engine.isRunning || stalledTicks >= Self.stalledTicksLimit {
            stopMonitor()
            deliver(.outputLost, pass: pass)
            return
        }
        if let hostPath, access(hostPath, F_OK) != 0, errno == ENOENT {
            stopMonitor()
            deliver(.fileGone, pass: pass)
        }
    }

    // MARK: - Testing

    /// What the engine posts when the hardware changes under it.
    func postConfigurationChangeForTesting() {
        queue.async { [self] in
            guard let engine else { return }
            NotificationCenter.default.post(name: .AVAudioEngineConfigurationChange, object: engine)
        }
    }

    /// What someone else deactivating the session does: the output stops, and nothing says so.
    func dropOutputForTesting() {
        queue.async { [self] in
            engine?.stop()
        }
    }
}

/// The queue, the transport and the modes. Main thread.
@MainActor
final class WorkspaceAudioEngine {
    static let shared = WorkspaceAudioEngine()

    private static let volumeKey = "com.pscal.workspace.music.volume"
    private static let repeatKey = "com.pscal.workspace.music.repeat"
    private static let shuffleKey = "com.pscal.workspace.music.shuffle"
    /// Restarts this close together mean the output isn't coming back.
    private static let restartLimit = 5
    private static let restartWindow: CFTimeInterval = 20

    private let pipeline = WorkspaceAudioPipeline()
    private var observers: [NSObjectProtocol] = []

    private(set) var queue: [WorkspaceAudioTrack] = []
    /// -1 when the queue is empty.
    private(set) var currentIndex = -1
    private(set) var state = WorkspaceAudioPlaybackState.stopped
    /// What the player last had to say, such as a file it couldn't play and moved past. Stays
    /// until the person does something else, since playback has already moved on from it.
    private(set) var statusMessage: String?
    private(set) var duration: TimeInterval = 0

    private var currentPass = WorkspaceFileOperation()
    private var playRequest: WorkspaceFileOperation?
    private var isLoaded = false
    private var wasPlayingBeforeInterruption = false
    private var shuffleHistory: [Int] = []
    /// Consecutive tracks that wouldn't play, so a queue of broken files can't spin for ever.
    private var loadFailureStreak = 0
    private var restartPending = false
    private var restartCount = 0
    private var lastRestart: CFTimeInterval = 0
    private var totalRestarts = 0

    var volume: Float {
        didSet {
            volume = max(0, min(1, volume))
            pipeline.setVolume(volume)
            UserDefaults.standard.set(volume, forKey: Self.volumeKey)
        }
    }

    var repeatMode: WorkspaceAudioRepeatMode {
        didSet {
            UserDefaults.standard.set(repeatMode.rawValue, forKey: Self.repeatKey)
        }
    }

    var shuffleEnabled: Bool {
        didSet {
            shuffleHistory.removeAll()
            UserDefaults.standard.set(shuffleEnabled, forKey: Self.shuffleKey)
        }
    }

    var currentTrack: WorkspaceAudioTrack? {
        queue.indices.contains(currentIndex) ? queue[currentIndex] : nil
    }

    var currentTime: TimeInterval {
        let position = pipeline.position
        return duration > 0 ? min(position, duration) : position
    }

    var isPlayingForTesting: Bool { state == .playing }
    var currentPathForTesting: String? { currentTrack?.path }
    var trackCountForTesting: Int { queue.count }
    var statusMessageForTesting: String? { statusMessage }
    var currentTimeForTesting: TimeInterval { currentTime }
    /// Whether Music is holding the audio session, which it should only be while playing.
    var isSessionActiveForTesting: Bool { WorkspaceAudioSessionSharing.musicHoldsSession }
    /// How many times the output has been restarted by anything but the person.
    var outputRestartsForTesting: Int { totalRestarts }

    func postConfigurationChangeForTesting() {
        pipeline.postConfigurationChangeForTesting()
    }

    func dropOutputForTesting() {
        pipeline.dropOutputForTesting()
    }

    private init() {
        let defaults = UserDefaults.standard
        volume = defaults.object(forKey: Self.volumeKey) == nil ? 1 : max(0, min(1, defaults.float(forKey: Self.volumeKey)))
        repeatMode = WorkspaceAudioRepeatMode(rawValue: defaults.integer(forKey: Self.repeatKey)) ?? .off
        shuffleEnabled = defaults.bool(forKey: Self.shuffleKey)

        let equalizer = WorkspaceAudioEqualizer.shared
        pipeline.configure(volume: volume, equalizerEnabled: equalizer.isEnabled, equalizerGains: equalizer.gains)
        pipeline.eventHandler = { [weak self] event, pass in
            self?.handle(event, pass: pass)
        }
        observeNotifications()
        setupRemoteCommands()
    }

    // MARK: - Queue

    /// Replaces the queue and plays from `startIndex`. With nil, stops and loads it without playing.
    func setQueue(_ tracks: [WorkspaceAudioTrack], startIndex: Int?) {
        statusMessage = nil
        queue = tracks
        shuffleHistory.removeAll()
        loadFailureStreak = 0
        post(.workspaceAudioQueueDidChange)
        if let startIndex, queue.indices.contains(startIndex) {
            selectTrack(at: startIndex)
        } else {
            stop()
            currentIndex = queue.isEmpty ? -1 : 0
            post(.workspaceAudioTrackDidChange)
        }
    }

    /// Adds to the end without disturbing what's playing.
    func enqueue(_ tracks: [WorkspaceAudioTrack]) {
        guard !tracks.isEmpty else { return }
        queue.append(contentsOf: tracks)
        if currentIndex < 0 {
            currentIndex = 0
            post(.workspaceAudioTrackDidChange)
        }
        post(.workspaceAudioQueueDidChange)
    }

    func removeTrack(at index: Int) {
        guard queue.indices.contains(index) else { return }
        let removingCurrent = index == currentIndex
        queue.remove(at: index)
        // The history holds indexes, which have just moved.
        shuffleHistory.removeAll()
        if index < currentIndex {
            currentIndex -= 1
        } else if removingCurrent {
            if currentIndex >= queue.count {
                currentIndex = queue.count - 1
            }
            if currentIndex >= 0 && state == .playing {
                selectTrack(at: currentIndex)
            } else {
                stop()
                post(.workspaceAudioTrackDidChange)
            }
        }
        post(.workspaceAudioQueueDidChange)
    }

    func clearQueue() {
        statusMessage = nil
        stop()
        queue.removeAll()
        shuffleHistory.removeAll()
        currentIndex = -1
        duration = 0
        post(.workspaceAudioQueueDidChange)
        post(.workspaceAudioTrackDidChange)
    }

    /// Something to say that isn't about playback, such as what a folder scan found.
    func setStatus(_ message: String?) {
        statusMessage = message
        post(.workspaceAudioStateDidChange)
    }

    // MARK: - Transport

    func playTrack(at index: Int) {
        statusMessage = nil
        loadFailureStreak = 0
        selectTrack(at: index)
    }

    private func selectTrack(at index: Int) {
        guard queue.indices.contains(index) else { return }
        currentIndex = index
        post(.workspaceAudioTrackDidChange)
        loadCurrentTrack(from: 0, autoPlay: true)
    }

    func play() {
        guard state != .playing else { return }
        guard currentTrack != nil else {
            if !queue.isEmpty {
                selectTrack(at: max(currentIndex, 0))
            }
            return
        }
        guard isLoaded else {
            // Nothing open: after a stop, or a track that failed.
            loadCurrentTrack(from: pipeline.position, autoPlay: true)
            return
        }
        startOutput()
    }

    func pause() {
        guard state == .playing else { return }
        playRequest?.cancel()
        playRequest = nil
        pipeline.pauseOutput()
        setState(.paused)
        updateNowPlayingInfo()
    }

    func togglePlayPause() {
        statusMessage = nil
        if state == .playing {
            pause()
        } else {
            play()
        }
        post(.workspaceAudioStateDidChange)
    }

    /// Halts and goes back to the start of the current track.
    func stop() {
        let pass = beginPass()
        isLoaded = false
        pipeline.stopOutput(pass: pass)
        setState(.stopped)
        updateNowPlayingInfo()
    }

    func next() {
        statusMessage = nil
        loadFailureStreak = 0
        advance()
    }

    /// More than 3 seconds in restarts this track, as music players do.
    func previous() {
        statusMessage = nil
        loadFailureStreak = 0
        guard currentTime <= 3 else {
            seek(to: 0)
            return
        }
        let index = indexAfterCurrent(forward: false)
        if index < 0 {
            seek(to: 0)
        } else {
            selectTrack(at: index)
        }
    }

    func seek(to time: TimeInterval) {
        guard time.isFinite else { return }
        let clamped = duration > 0 ? max(0, min(time, duration)) : max(0, time)
        guard isLoaded else {
            pipeline.setPosition(clamped)
            return
        }
        reposition(at: clamped, rebuild: false)
        updateNowPlayingInfo()
    }

    private func beginPass() -> WorkspaceFileOperation {
        currentPass.cancel()
        playRequest?.cancel()
        playRequest = nil
        let pass = WorkspaceFileOperation()
        currentPass = pass
        return pass
    }

    private func advance() {
        let index = indexAfterCurrent(forward: true)
        if index < 0 {
            stop()
        } else {
            selectTrack(at: index)
        }
    }

    /// -1 when there's nowhere to go. `pastFailure` moves on even under repeat-one, or a file
    /// that can't play would be tried again and again.
    private func indexAfterCurrent(forward: Bool, pastFailure: Bool = false) -> Int {
        let count = queue.count
        guard count > 0 else { return -1 }
        if repeatMode == .one && !pastFailure {
            return currentIndex
        }
        if shuffleEnabled {
            if forward {
                guard count > 1 else { return repeatMode == .off ? -1 : 0 }
                shuffleHistory.append(currentIndex)
                var pick = Int.random(in: 0..<count)
                while pick == currentIndex {
                    pick = Int.random(in: 0..<count)
                }
                return pick
            }
            return shuffleHistory.popLast() ?? -1
        }
        let next = currentIndex + (forward ? 1 : -1)
        if next >= count {
            return repeatMode == .off ? -1 : 0
        }
        if next < 0 {
            return repeatMode == .off ? -1 : count - 1
        }
        return next
    }

    // MARK: - Loading

    private func loadCurrentTrack(from start: TimeInterval, autoPlay: Bool) {
        guard let track = currentTrack else { return }
        let pass = beginPass()
        isLoaded = false
        setState(.loading)
        pipeline.load(path: track.path, from: start, pass: pass) { [weak self] outcome in
            guard let self, self.currentPass === pass else { return }
            switch outcome {
            case .failed(let message):
                self.handleLoadFailure(message)
            case .loaded(let loaded):
                self.isLoaded = true
                self.loadFailureStreak = 0
                self.duration = loaded.duration
                self.applyTags(loaded, toTrackAtPath: track.path)
                self.post(.workspaceAudioTrackDidChange)
                if autoPlay {
                    self.startOutput()
                } else {
                    self.setState(.paused)
                }
                self.updateNowPlayingInfo()
            }
        }
    }

    private func applyTags(_ loaded: WorkspaceAudioLoaded, toTrackAtPath path: String) {
        guard queue.indices.contains(currentIndex), queue[currentIndex].path == path else { return }
        if loaded.duration > 0 {
            queue[currentIndex].duration = loaded.duration
        }
        if let title = loaded.title, !title.isEmpty {
            queue[currentIndex].title = title
        }
        if let artist = loaded.artist, !artist.isEmpty {
            queue[currentIndex].artist = artist
        }
        if let album = loaded.album, !album.isEmpty {
            queue[currentIndex].album = album
        }
    }

    /// Says so and moves on, but stops once every track in the queue has failed, so repeating
    /// over a queue of broken files can't busy-loop.
    private func handleLoadFailure(_ message: String) {
        statusMessage = message
        loadFailureStreak += 1
        if queue.isEmpty || loadFailureStreak >= queue.count {
            loadFailureStreak = 0
            stop()
            return
        }
        let index = indexAfterCurrent(forward: true, pastFailure: true)
        if index < 0 {
            stop()
        } else {
            selectTrack(at: index)
        }
    }

    private func handle(_ event: WorkspaceAudioPipelineEvent, pass: WorkspaceFileOperation) {
        guard pass === currentPass else { return }
        let name = currentTrack.map { ($0.path as NSString).lastPathComponent } ?? "This file"
        switch event {
        case .ended(let readFailed):
            if readFailed {
                setStatus("“\(name)” couldn’t be read to the end.")
            }
            if repeatMode == .one && !readFailed {
                loadCurrentTrack(from: 0, autoPlay: true)
            } else {
                advance()
            }
        case .fileGone:
            handleLoadFailure("“\(name)” isn’t there any more, so it was skipped.")
        case .outputLost:
            if state == .playing {
                scheduleRestart()
            }
        }
    }

    // MARK: - Output

    private func startOutput() {
        setState(.playing)
        let request = WorkspaceFileOperation()
        playRequest?.cancel()
        playRequest = request
        let pass = currentPass
        pipeline.startOutput(pass: pass, request: request) { [weak self] failure in
            guard let self, self.currentPass === pass, self.playRequest === request else { return }
            if let failure {
                self.outputFailed(failure)
            }
            self.updateNowPlayingInfo()
        }
    }

    private func outputFailed(_ message: String) {
        statusMessage = message
        guard state == .playing else {
            post(.workspaceAudioStateDidChange)
            return
        }
        pipeline.pauseOutput()
        setState(.paused)
    }

    private func reposition(at time: TimeInterval, rebuild: Bool) {
        let wasPlaying = state == .playing
        let pass = beginPass()
        let request = WorkspaceFileOperation()
        playRequest = wasPlaying ? request : nil
        pipeline.reposition(to: time, play: wasPlaying, rebuild: rebuild, pass: pass, request: request) { [weak self] failure in
            guard let self, self.currentPass === pass else { return }
            if let failure {
                self.outputFailed(failure)
            }
            self.updateNowPlayingInfo()
        }
    }

    /// One restart for a burst of changes, once they've settled.
    private func scheduleRestart() {
        guard !restartPending else { return }
        restartPending = true
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.15) { [weak self] in
            MainActor.assumeIsolated {
                self?.restartOutput()
            }
        }
    }

    private func restartOutput() {
        restartPending = false
        guard state == .playing, isLoaded else { return }
        let now = CACurrentMediaTime()
        if now - lastRestart > Self.restartWindow {
            restartCount = 0
        }
        lastRestart = now
        restartCount += 1
        guard restartCount <= Self.restartLimit else {
            restartCount = 0
            outputFailed("Audio output keeps dropping, so Music has paused.")
            return
        }
        totalRestarts += 1
        reposition(at: currentTime, rebuild: true)
    }

    // MARK: - Session events

    private func observeNotifications() {
        let center = NotificationCenter.default
        observers.append(center.addObserver(forName: AVAudioSession.interruptionNotification, object: nil, queue: .main) { [weak self] note in
            let type = note.userInfo?[AVAudioSessionInterruptionTypeKey] as? UInt
            let options = note.userInfo?[AVAudioSessionInterruptionOptionKey] as? UInt
            MainActor.assumeIsolated {
                self?.handleInterruption(type: type, options: options)
            }
        })
        observers.append(center.addObserver(forName: AVAudioSession.routeChangeNotification, object: nil, queue: .main) { [weak self] note in
            let reason = note.userInfo?[AVAudioSessionRouteChangeReasonKey] as? UInt
            MainActor.assumeIsolated {
                // Headphones unplugged, and music shouldn't carry on from the speaker.
                if reason == AVAudioSession.RouteChangeReason.oldDeviceUnavailable.rawValue {
                    self?.pause()
                }
            }
        })
        observers.append(center.addObserver(forName: AVAudioSession.mediaServicesWereResetNotification, object: nil, queue: .main) { [weak self] _ in
            MainActor.assumeIsolated {
                guard let self else { return }
                self.pipeline.discardGraph()
                if self.state == .playing {
                    self.scheduleRestart()
                }
            }
        })
        observers.append(center.addObserver(forName: .workspaceEqualizerDidChange, object: nil, queue: .main) { [weak self] _ in
            MainActor.assumeIsolated {
                let equalizer = WorkspaceAudioEqualizer.shared
                self?.pipeline.setEqualizer(enabled: equalizer.isEnabled, gains: equalizer.gains)
            }
        })
    }

    private func handleInterruption(type: UInt?, options: UInt?) {
        guard let type, let kind = AVAudioSession.InterruptionType(rawValue: type) else { return }
        switch kind {
        case .began:
            wasPlayingBeforeInterruption = state == .playing
            pause()
        case .ended:
            let shouldResume = AVAudioSession.InterruptionOptions(rawValue: options ?? 0).contains(.shouldResume)
            if shouldResume && wasPlayingBeforeInterruption {
                play()
            }
            wasPlayingBeforeInterruption = false
        @unknown default:
            break
        }
    }

    // MARK: - Now Playing

    private func setupRemoteCommands() {
        let center = MPRemoteCommandCenter.shared()
        func add(_ command: MPRemoteCommand, _ action: @escaping @MainActor (WorkspaceAudioEngine) -> Void) {
            command.addTarget { [weak self] _ in
                DispatchQueue.main.async {
                    MainActor.assumeIsolated {
                        if let self {
                            action(self)
                        }
                    }
                }
                return .success
            }
        }
        add(center.playCommand) { $0.play() }
        add(center.pauseCommand) { $0.pause() }
        add(center.togglePlayPauseCommand) { $0.togglePlayPause() }
        add(center.nextTrackCommand) { $0.next() }
        add(center.previousTrackCommand) { $0.previous() }
        center.changePlaybackPositionCommand.isEnabled = true
        center.changePlaybackPositionCommand.addTarget { [weak self] event in
            guard let event = event as? MPChangePlaybackPositionCommandEvent else { return .commandFailed }
            let position = event.positionTime
            DispatchQueue.main.async {
                MainActor.assumeIsolated {
                    self?.seek(to: position)
                }
            }
            return .success
        }
    }

    private func updateNowPlayingInfo() {
        guard let track = currentTrack else {
            MPNowPlayingInfoCenter.default().nowPlayingInfo = nil
            return
        }
        var info: [String: Any] = [
            MPMediaItemPropertyTitle: track.title,
            MPNowPlayingInfoPropertyElapsedPlaybackTime: currentTime,
            MPNowPlayingInfoPropertyPlaybackRate: state == .playing ? 1.0 : 0.0,
        ]
        info[MPMediaItemPropertyArtist] = track.artist
        info[MPMediaItemPropertyAlbumTitle] = track.album
        if duration > 0 {
            info[MPMediaItemPropertyPlaybackDuration] = duration
        }
        MPNowPlayingInfoCenter.default().nowPlayingInfo = info
    }

    // MARK: - Helpers

    private func setState(_ newState: WorkspaceAudioPlaybackState) {
        guard state != newState else {
            // A message can change while the state doesn't.
            post(.workspaceAudioStateDidChange)
            return
        }
        state = newState
        post(.workspaceAudioStateDidChange)
    }

    private func post(_ name: Notification.Name) {
        NotificationCenter.default.post(name: name, object: self)
    }
}

/// The Music player's 10-band graphic equalizer, an `AVAudioUnitEQ` between the player and the
/// mixer. Settings persist and apply at once, whether or not Music has ever been opened: the
/// player reads them when it starts, and follows them after that. Main thread.
@MainActor
final class WorkspaceAudioEqualizer {
    static let shared = WorkspaceAudioEqualizer()

    /// Band centres in Hz, an octave apart.
    nonisolated static let frequencies: [Float] = [31, 62, 125, 250, 500, 1000, 2000, 4000, 8000, 16000]
    static let presetNames = ["Flat", "Bass Boost", "Treble Boost", "Vocal", "Loudness", "Rock"]

    private static let enabledKey = "com.pscal.workspace.equalizer.enabled"
    private static let gainsKey = "com.pscal.workspace.equalizer.gains"
    private static let presetKey = "com.pscal.workspace.equalizer.preset"
    private static let maximumGain: Float = 12
    /// dB per band, 31 Hz to 16 kHz.
    private static let presets: [String: [Float]] = [
        "Flat": [0, 0, 0, 0, 0, 0, 0, 0, 0, 0],
        "Bass Boost": [6, 5, 4, 2, 0, 0, 0, 0, 0, 0],
        "Treble Boost": [0, 0, 0, 0, 0, 1, 2, 4, 5, 6],
        "Vocal": [-2, -2, -1, 1, 3, 4, 3, 1, 0, -1],
        "Loudness": [5, 4, 2, 0, -1, -1, 0, 2, 4, 5],
        "Rock": [4, 3, 1, -1, -2, -1, 1, 3, 4, 4],
    ]

    var isEnabled: Bool {
        didSet { changed() }
    }
    private(set) var gains: [Float]
    private(set) var currentPresetName: String

    private init() {
        let defaults = UserDefaults.standard
        isEnabled = defaults.bool(forKey: Self.enabledKey)
        let stored = defaults.array(forKey: Self.gainsKey) ?? []
        gains = Self.frequencies.indices.map { index in
            guard index < stored.count, let value = stored[index] as? NSNumber else { return 0 }
            return max(-Self.maximumGain, min(Self.maximumGain, value.floatValue))
        }
        currentPresetName = defaults.string(forKey: Self.presetKey) ?? "Flat"
    }

    func gain(forBand band: Int) -> Float {
        gains.indices.contains(band) ? gains[band] : 0
    }

    /// Clamped to ±12 dB. Editing a band makes the preset "Custom".
    func setGain(_ gain: Float, forBand band: Int) {
        guard gains.indices.contains(band) else { return }
        gains[band] = max(-Self.maximumGain, min(Self.maximumGain, gain))
        currentPresetName = "Custom"
        changed()
    }

    func applyPreset(_ name: String) {
        guard let preset = Self.presets[name] else { return }
        gains = preset
        currentPresetName = name
        changed()
    }

    private func changed() {
        let defaults = UserDefaults.standard
        defaults.set(isEnabled, forKey: Self.enabledKey)
        defaults.set(gains, forKey: Self.gainsKey)
        defaults.set(currentPresetName, forKey: Self.presetKey)
        NotificationCenter.default.post(name: .workspaceEqualizerDidChange, object: self)
    }
}
