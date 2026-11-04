// Copyright 2026 Mike Miller
// SPDX-License-Identifier: Apache-2.0
//
// Ported by its author from iSH-AOK's Workspace (app/WorkspaceViewController.m at 0f75fee1c)
// and licensed to PSCAL under the Apache License 2.0.

import UIKit

private func workspaceMusicColor(_ hex: UInt32) -> UIColor {
    UIColor(red: CGFloat((hex >> 16) & 0xFF) / 255, green: CGFloat((hex >> 8) & 0xFF) / 255, blue: CGFloat(hex & 0xFF) / 255, alpha: 1)
}

/// The animated rainbow on the player's "screen". Cheap: a low-rate timer nudges bar heights,
/// and the player only runs it while it's playing and on screen.
@MainActor
private final class WorkspaceMusicBarsView: UIView {
    private var heights: [NSLayoutConstraint] = []
    private var timer: Timer?

    override init(frame: CGRect) {
        super.init(frame: frame)
        let colors: [UInt32] = [
            0xFF4D4D, 0xFF8A3C, 0xFFD23C, 0xB6E83C, 0x4FE06A, 0x36D6A0, 0x33C9D6,
            0x3AA0FF, 0x6B7BFF, 0xA86BFF, 0xE25BFF, 0xFF5BBF, 0xFF4D7A,
        ]
        let row = UIStackView()
        row.translatesAutoresizingMaskIntoConstraints = false
        row.axis = .horizontal
        row.alignment = .bottom
        row.distribution = .fillEqually
        row.spacing = 3
        addSubview(row)
        NSLayoutConstraint.activate([
            row.leadingAnchor.constraint(equalTo: leadingAnchor),
            row.trailingAnchor.constraint(equalTo: trailingAnchor),
            row.topAnchor.constraint(equalTo: topAnchor),
            row.bottomAnchor.constraint(equalTo: bottomAnchor),
        ])
        for color in colors {
            let bar = UIView()
            bar.translatesAutoresizingMaskIntoConstraints = false
            bar.backgroundColor = workspaceMusicColor(color)
            bar.layer.cornerRadius = 1
            row.addArrangedSubview(bar)
            let height = bar.heightAnchor.constraint(equalToConstant: 5)
            height.isActive = true
            heights.append(height)
        }
    }

    @available(*, unavailable)
    required init?(coder: NSCoder) {
        fatalError("init(coder:) is not used")
    }

    deinit {
        timer?.invalidate()
    }

    func setActive(_ active: Bool) {
        guard active != (timer != nil) else { return }
        timer?.invalidate()
        timer = nil
        if active {
            timer = Timer.scheduledTimer(withTimeInterval: 0.13, repeats: true) { [weak self] _ in
                MainActor.assumeIsolated {
                    self?.tick()
                }
            }
            tick()
        } else {
            heights.forEach { $0.constant = 5 }
            UIView.animate(withDuration: 0.2) {
                self.layoutIfNeeded()
            }
        }
    }

    private func tick() {
        let maxHeight = max(bounds.height, 10)
        for height in heights {
            height.constant = 4 + CGFloat.random(in: 0..<(maxHeight - 4))
        }
        UIView.animate(withDuration: 0.11, delay: 0, options: [.beginFromCurrentState, .curveEaseOut]) {
            self.layoutIfNeeded()
        }
    }
}

/// A remote control for the app's one player, drawn as a device: a dark screen, a click wheel and
/// a volume slider, with the queue below. Closing the window leaves the music playing. Cmd+= and
/// friends scale the queue list, which is what a person reads here; the device is a drawing of a
/// player in fixed-size boxes and stays as it is.
@MainActor
final class WorkspaceMusicAppletViewController: WorkspaceAppletViewController, WorkspaceFileOpenable, WorkspaceTextScalable, UITableViewDataSource, UITableViewDelegate {
    private static let rowReuseIdentifier = "WorkspaceMusicRow"

    /// The player is a fixed-width device. The window hugs it, with a thin margin each side.
    static var deviceWidth: CGFloat { WorkspaceMetrics.usesPhoneLayout ? 240 : 200 }
    static var windowWidth: CGFloat { deviceWidth + 12 }

    private let deviceBody = UIView()
    private let wheelView = UIView()
    private let counterLabel = UILabel()
    private let barsView = WorkspaceMusicBarsView(frame: .zero)
    private let titleLabel = UILabel()
    private let progressSlider = UISlider()
    private let elapsedLabel = UILabel()
    private let durationLabel = UILabel()
    private let shuffleButton = UIButton(type: .system)
    private let repeatButton = UIButton(type: .system)
    private let menuButton = UIButton(type: .system)
    private let previousButton = UIButton(type: .system)
    private let playButton = UIButton(type: .system)
    private let nextButton = UIButton(type: .system)
    private let volumeButton = UIButton(type: .system)
    private let speakerView = UIImageView()
    private let volumeSlider = UISlider()
    private let statusRow = UIStackView()
    private let statusLabel = UILabel()
    private let cancelScanButton = UIButton(type: .system)
    private let tableView = UITableView(frame: .zero, style: .plain)

    private var volumeBeforeMute: Float = 0
    private var progressTimer: Timer?
    private var scrubbing = false
    private var observers: [NSObjectProtocol] = []

    private var engine: WorkspaceAudioEngine { .shared }
    private var library: WorkspaceAudioLibrary { .shared }

    var isPlayingForTesting: Bool { engine.isPlayingForTesting }
    var currentPathForTesting: String? { engine.currentPathForTesting }
    var trackCountForTesting: Int { engine.trackCountForTesting }
    var statusMessageForTesting: String? { engine.statusMessageForTesting }
    var isScanningForTesting: Bool { library.isScanning }

    private var trackRowHeight: CGFloat {
        // The rows are a fixed height, so it grows with the fonts or a two-line row clips.
        (38 * workspaceTextScale).rounded(.up)
    }

    deinit {
        progressTimer?.invalidate()
        observers.forEach { NotificationCenter.default.removeObserver($0) }
    }

    override func viewDidLoad() {
        super.viewDidLoad()
        buildDevice()
        buildList()

        let names: [Notification.Name] = [.workspaceAudioStateDidChange, .workspaceAudioTrackDidChange, .workspaceAudioQueueDidChange, .workspaceAudioLibraryDidChange]
        for name in names {
            observers.append(NotificationCenter.default.addObserver(forName: name, object: nil, queue: .main) { [weak self] _ in
                MainActor.assumeIsolated {
                    self?.engineDidChange()
                }
            })
        }
        applyTheme()
        refreshNowPlaying()
        refreshTransport()
        refreshStatus()
    }

    override func viewDidLayoutSubviews() {
        super.viewDidLayoutSubviews()
        wheelView.layer.cornerRadius = wheelView.bounds.width / 2
    }

    override func viewWillAppear(_ animated: Bool) {
        super.viewWillAppear(animated)
        // Grows and shrinks up and down only, as the device has one width.
        workspaceHost?.window(holding: self)?.maximumSize = CGSize(width: Self.windowWidth, height: 0)
        progressTimer?.invalidate()
        progressTimer = Timer.scheduledTimer(withTimeInterval: 0.4, repeats: true) { [weak self] _ in
            MainActor.assumeIsolated {
                self?.progressTick()
            }
        }
        tableView.reloadData()
        refreshNowPlaying()
        refreshTransport()
        refreshStatus()
    }

    override func viewWillDisappear(_ animated: Bool) {
        super.viewWillDisappear(animated)
        // Playback keeps running; this only stops polling it for the interface.
        progressTimer?.invalidate()
        progressTimer = nil
        barsView.setActive(false)
    }

    // MARK: - Building

    private func buildDevice() {
        deviceBody.translatesAutoresizingMaskIntoConstraints = false
        deviceBody.layer.cornerRadius = 18
        toolContentView.addSubview(deviceBody)

        // Always a dark display, whatever the theme.
        let screen = UIView()
        screen.translatesAutoresizingMaskIntoConstraints = false
        screen.backgroundColor = workspaceMusicColor(0x10141B)
        screen.layer.cornerRadius = 10
        screen.clipsToBounds = true
        deviceBody.addSubview(screen)

        configureDeviceButton(shuffleButton, symbol: "shuffle", text: "S", pointSize: 12, label: "Shuffle",
                              identifier: "workspace.music.shuffle", action: #selector(toggleShuffle))
        configureDeviceButton(repeatButton, symbol: "repeat", text: "R", pointSize: 12, label: "Repeat",
                              identifier: "workspace.music.repeat", action: #selector(cycleRepeat))
        shuffleButton.setContentHuggingPriority(.required, for: .horizontal)
        repeatButton.setContentHuggingPriority(.required, for: .horizontal)
        counterLabel.font = .monospacedDigitSystemFont(ofSize: 10, weight: .regular)
        counterLabel.textColor = workspaceMusicColor(0xE8EEF6)
        counterLabel.textAlignment = .center
        counterLabel.text = "0 / 0"
        counterLabel.accessibilityIdentifier = "workspace.music.counter"
        let topRow = UIStackView(arrangedSubviews: [shuffleButton, counterLabel, repeatButton])
        topRow.axis = .horizontal
        topRow.alignment = .center

        barsView.translatesAutoresizingMaskIntoConstraints = false
        barsView.heightAnchor.constraint(equalToConstant: 20).isActive = true

        titleLabel.font = .systemFont(ofSize: 11, weight: .medium)
        titleLabel.textColor = .white
        titleLabel.textAlignment = .center
        titleLabel.lineBreakMode = .byTruncatingTail
        titleLabel.text = "Nothing playing"
        titleLabel.accessibilityIdentifier = "workspace.music.title"

        progressSlider.isContinuous = true
        progressSlider.maximumTrackTintColor = workspaceMusicColor(0x3A3F49)
        progressSlider.accessibilityLabel = "Position"
        progressSlider.accessibilityIdentifier = "workspace.music.progress"
        progressSlider.addTarget(self, action: #selector(scrubStarted), for: .touchDown)
        progressSlider.addTarget(self, action: #selector(scrubChanged), for: .valueChanged)
        progressSlider.addTarget(self, action: #selector(scrubEnded), for: [.touchUpInside, .touchUpOutside, .touchCancel])

        configureScreenTimeLabel(elapsedLabel, alignment: .left, identifier: "workspace.music.elapsed")
        configureScreenTimeLabel(durationLabel, alignment: .right, identifier: "workspace.music.duration")
        let timeRow = UIStackView(arrangedSubviews: [elapsedLabel, durationLabel])
        timeRow.axis = .horizontal
        timeRow.distribution = .fillEqually

        let screenStack = UIStackView(arrangedSubviews: [topRow, barsView, titleLabel, progressSlider, timeRow])
        screenStack.axis = .vertical
        screenStack.spacing = 4
        screenStack.translatesAutoresizingMaskIntoConstraints = false
        screen.addSubview(screenStack)
        NSLayoutConstraint.activate([
            screenStack.topAnchor.constraint(equalTo: screen.topAnchor, constant: 7),
            screenStack.leadingAnchor.constraint(equalTo: screen.leadingAnchor, constant: 9),
            screenStack.trailingAnchor.constraint(equalTo: screen.trailingAnchor, constant: -9),
            screenStack.bottomAnchor.constraint(equalTo: screen.bottomAnchor, constant: -7),
        ])

        wheelView.translatesAutoresizingMaskIntoConstraints = false
        wheelView.layer.borderWidth = 1
        deviceBody.addSubview(wheelView)

        configureDeviceButton(menuButton, symbol: nil, text: "M", pointSize: 11, label: "Menu",
                              identifier: "workspace.music.menu", action: #selector(menuTapped))
        configureDeviceButton(previousButton, symbol: "backward.fill", text: "|<", pointSize: 11, label: "Previous Track",
                              identifier: "workspace.music.previous", action: #selector(previousTapped))
        configureDeviceButton(nextButton, symbol: "forward.fill", text: ">|", pointSize: 11, label: "Next Track",
                              identifier: "workspace.music.next", action: #selector(nextTapped))
        configureDeviceButton(volumeButton, symbol: nil, text: "VOL", pointSize: 9, label: "Volume",
                              identifier: "workspace.music.volume", action: #selector(volumeTapped))
        volumeButton.titleLabel?.font = .systemFont(ofSize: 9, weight: .medium)
        configureDeviceButton(playButton, symbol: "play.fill", text: ">", pointSize: 26, label: "Play/Pause",
                              identifier: "workspace.music.play", action: #selector(playPauseTapped))
        playButton.layer.cornerRadius = 23
        playButton.layer.borderWidth = 1
        for control in [menuButton, previousButton, nextButton, volumeButton, playButton] {
            wheelView.addSubview(control)
        }
        NSLayoutConstraint.activate([
            menuButton.centerXAnchor.constraint(equalTo: wheelView.centerXAnchor),
            menuButton.topAnchor.constraint(equalTo: wheelView.topAnchor, constant: 7),
            previousButton.centerYAnchor.constraint(equalTo: wheelView.centerYAnchor),
            previousButton.leadingAnchor.constraint(equalTo: wheelView.leadingAnchor, constant: 9),
            nextButton.centerYAnchor.constraint(equalTo: wheelView.centerYAnchor),
            nextButton.trailingAnchor.constraint(equalTo: wheelView.trailingAnchor, constant: -9),
            volumeButton.centerXAnchor.constraint(equalTo: wheelView.centerXAnchor),
            volumeButton.bottomAnchor.constraint(equalTo: wheelView.bottomAnchor, constant: -8),
            playButton.centerXAnchor.constraint(equalTo: wheelView.centerXAnchor),
            playButton.centerYAnchor.constraint(equalTo: wheelView.centerYAnchor),
            playButton.widthAnchor.constraint(equalToConstant: 46),
            playButton.heightAnchor.constraint(equalToConstant: 46),
        ])

        speakerView.translatesAutoresizingMaskIntoConstraints = false
        speakerView.contentMode = .scaleAspectFit
        speakerView.image = UIImage(systemName: "speaker.fill")
        speakerView.setContentHuggingPriority(.required, for: .horizontal)
        speakerView.widthAnchor.constraint(equalToConstant: 13).isActive = true
        volumeSlider.minimumValue = 0
        volumeSlider.maximumValue = 1
        volumeSlider.value = engine.volume
        volumeSlider.accessibilityLabel = "Volume"
        volumeSlider.accessibilityIdentifier = "workspace.music.volumeSlider"
        volumeSlider.addTarget(self, action: #selector(volumeChanged), for: .valueChanged)
        let volumeRow = UIStackView(arrangedSubviews: [speakerView, volumeSlider])
        volumeRow.axis = .horizontal
        volumeRow.alignment = .center
        volumeRow.spacing = 6
        volumeRow.translatesAutoresizingMaskIntoConstraints = false
        deviceBody.addSubview(volumeRow)

        let pad: CGFloat = 11
        NSLayoutConstraint.activate([
            screen.topAnchor.constraint(equalTo: deviceBody.topAnchor, constant: pad),
            screen.leadingAnchor.constraint(equalTo: deviceBody.leadingAnchor, constant: pad),
            screen.trailingAnchor.constraint(equalTo: deviceBody.trailingAnchor, constant: -pad),
            wheelView.topAnchor.constraint(equalTo: screen.bottomAnchor, constant: 11),
            wheelView.centerXAnchor.constraint(equalTo: deviceBody.centerXAnchor),
            wheelView.widthAnchor.constraint(equalTo: deviceBody.widthAnchor, multiplier: 0.6),
            wheelView.heightAnchor.constraint(equalTo: wheelView.widthAnchor),
            volumeRow.topAnchor.constraint(equalTo: wheelView.bottomAnchor, constant: 8),
            volumeRow.leadingAnchor.constraint(equalTo: deviceBody.leadingAnchor, constant: pad),
            volumeRow.trailingAnchor.constraint(equalTo: deviceBody.trailingAnchor, constant: -pad),
            volumeRow.bottomAnchor.constraint(equalTo: deviceBody.bottomAnchor, constant: -pad),
        ])

        // Fills the window's width, capped and centred.
        let inset: CGFloat = 6
        let fillWidth = deviceBody.widthAnchor.constraint(equalTo: toolContentView.widthAnchor, constant: -2 * inset)
        fillWidth.priority = .defaultHigh
        NSLayoutConstraint.activate([
            deviceBody.topAnchor.constraint(equalTo: toolContentView.topAnchor, constant: inset),
            deviceBody.centerXAnchor.constraint(equalTo: toolContentView.centerXAnchor),
            deviceBody.leadingAnchor.constraint(greaterThanOrEqualTo: toolContentView.leadingAnchor, constant: inset),
            deviceBody.widthAnchor.constraint(lessThanOrEqualToConstant: Self.deviceWidth),
            fillWidth,
        ])
    }

    /// What the player has to say, then the queue, under the device.
    private func buildList() {
        statusLabel.numberOfLines = 2
        statusLabel.lineBreakMode = .byTruncatingMiddle
        statusLabel.font = .systemFont(ofSize: WorkspaceTheme.fontSize(.caption1))
        statusLabel.accessibilityIdentifier = "workspace.music.status"
        cancelScanButton.setTitle("Cancel", for: .normal)
        cancelScanButton.titleLabel?.font = .systemFont(ofSize: WorkspaceTheme.fontSize(.caption1), weight: .semibold)
        cancelScanButton.setContentHuggingPriority(.required, for: .horizontal)
        cancelScanButton.setContentCompressionResistancePriority(.required, for: .horizontal)
        cancelScanButton.accessibilityIdentifier = "workspace.music.cancelScan"
        cancelScanButton.addTarget(self, action: #selector(cancelScanTapped), for: .touchUpInside)
        statusRow.addArrangedSubview(statusLabel)
        statusRow.addArrangedSubview(cancelScanButton)
        statusRow.axis = .horizontal
        statusRow.spacing = 6
        statusRow.alignment = .center
        statusRow.isLayoutMarginsRelativeArrangement = true
        statusRow.directionalLayoutMargins = NSDirectionalEdgeInsets(top: 0, leading: 10, bottom: 0, trailing: 10)

        tableView.dataSource = self
        tableView.delegate = self
        tableView.backgroundColor = .clear
        tableView.rowHeight = trackRowHeight
        tableView.alwaysBounceVertical = true
        tableView.tableFooterView = UIView()
        tableView.accessibilityIdentifier = "workspace.music.table"

        let list = UIStackView(arrangedSubviews: [statusRow, tableView])
        list.translatesAutoresizingMaskIntoConstraints = false
        list.axis = .vertical
        list.spacing = 4
        toolContentView.addSubview(list)
        // The device is tall. In a window shorter than it, the list gives way rather than the
        // two conflicting.
        let listTop = list.topAnchor.constraint(equalTo: deviceBody.bottomAnchor, constant: 8)
        listTop.priority = .defaultHigh
        NSLayoutConstraint.activate([
            listTop,
            list.leadingAnchor.constraint(equalTo: toolContentView.leadingAnchor),
            list.trailingAnchor.constraint(equalTo: toolContentView.trailingAnchor),
            list.bottomAnchor.constraint(equalTo: toolContentView.bottomAnchor),
        ])
    }

    private func configureDeviceButton(_ button: UIButton, symbol: String?, text: String, pointSize: CGFloat, label: String, identifier: String, action: Selector) {
        button.translatesAutoresizingMaskIntoConstraints = false
        button.accessibilityLabel = label
        button.accessibilityIdentifier = identifier
        if let symbol, let image = UIImage(systemName: symbol, withConfiguration: UIImage.SymbolConfiguration(pointSize: pointSize, weight: .semibold)) {
            button.setImage(image, for: .normal)
        } else {
            button.setTitle(text, for: .normal)
            button.titleLabel?.font = .systemFont(ofSize: pointSize, weight: .semibold)
        }
        button.addTarget(self, action: action, for: .touchUpInside)
    }

    private func configureScreenTimeLabel(_ label: UILabel, alignment: NSTextAlignment, identifier: String) {
        label.font = .monospacedDigitSystemFont(ofSize: 9, weight: .regular)
        label.textColor = workspaceMusicColor(0xAEB6C2)
        label.textAlignment = alignment
        label.text = "0:00"
        label.accessibilityIdentifier = identifier
    }

    private func setSymbol(_ symbol: String, on button: UIButton, pointSize: CGFloat) {
        button.setImage(UIImage(systemName: symbol, withConfiguration: UIImage.SymbolConfiguration(pointSize: pointSize, weight: .semibold)), for: .normal)
    }

    // MARK: - Refreshing

    private func engineDidChange() {
        refreshNowPlaying()
        refreshTransport()
        refreshStatus()
        tableView.reloadData()
    }

    private func refreshNowPlaying() {
        let total = engine.queue.count
        let index = engine.currentIndex
        counterLabel.text = total > 0 && index >= 0 ? "\(index + 1) / \(total)" : "0 / \(total)"
        if let track = engine.currentTrack {
            titleLabel.text = track.artist.map { $0.isEmpty ? track.title : "\(track.title) — \($0)" } ?? track.title
        } else {
            titleLabel.text = "Nothing playing"
        }
        let duration = engine.duration
        durationLabel.text = timeString(duration)
        progressSlider.maximumValue = Float(duration > 0 ? duration : 1)
        progressSlider.isEnabled = duration > 0
        progressTick()
    }

    private func refreshTransport() {
        let playing = engine.state == .playing
        setSymbol(playing ? "pause.fill" : "play.fill", on: playButton, pointSize: 26)
        playButton.accessibilityValue = playing ? "Playing" : "Not playing"

        let activeTint = theme.accent
        let inactiveTint = workspaceMusicColor(0x5A6472)
        shuffleButton.tintColor = engine.shuffleEnabled ? activeTint : inactiveTint
        shuffleButton.accessibilityValue = engine.shuffleEnabled ? "On" : "Off"
        repeatButton.tintColor = engine.repeatMode == .off ? inactiveTint : activeTint
        switch engine.repeatMode {
        case .off:
            setSymbol("repeat", on: repeatButton, pointSize: 15)
            repeatButton.accessibilityValue = "Off"
        case .all:
            setSymbol("repeat", on: repeatButton, pointSize: 15)
            repeatButton.accessibilityValue = "All"
        case .one:
            setSymbol("repeat.1", on: repeatButton, pointSize: 15)
            repeatButton.accessibilityValue = "One"
        }
        volumeSlider.value = engine.volume
        barsView.setActive(playing && progressTimer != nil)
    }

    private func refreshStatus() {
        let scanning = library.isScanning
        let message = scanning ? "Looking for audio…" : engine.statusMessage
        statusLabel.text = message
        statusRow.isHidden = message == nil
        cancelScanButton.isHidden = !scanning
    }

    private func progressTick() {
        guard !scrubbing else { return }
        let time = engine.currentTime
        if engine.duration > 0 {
            progressSlider.value = Float(time)
        }
        elapsedLabel.text = timeString(time)
    }

    private func timeString(_ time: TimeInterval) -> String {
        let total = time.isFinite && time > 0 ? Int(time.rounded()) : 0
        return String(format: "%d:%02d", total / 60, total % 60)
    }

    // MARK: - Theme

    /// The device follows the theme, so the player matches the rest of Workspace. Its screen
    /// stays a dark display and its bars stay a rainbow.
    override func applyTheme() {
        super.applyTheme()
        let colors = theme
        // The theme's text colours are only guaranteed against its card surfaces.
        toolContentView.backgroundColor = colors.card.withAlphaComponent(0.95)
        deviceBody.backgroundColor = colors.accent
        wheelView.backgroundColor = colors.card
        wheelView.layer.borderColor = colors.stroke.cgColor
        playButton.backgroundColor = colors.cardAlt
        playButton.layer.borderColor = colors.stroke.cgColor
        playButton.tintColor = colors.primary
        for button in [menuButton, previousButton, nextButton, volumeButton] {
            button.tintColor = colors.primary
            button.setTitleColor(colors.primary, for: .normal)
        }
        speakerView.tintColor = colors.primary
        volumeSlider.minimumTrackTintColor = colors.primary
        volumeSlider.maximumTrackTintColor = colors.stroke
        progressSlider.minimumTrackTintColor = colors.accent
        statusLabel.textColor = colors.secondary
        cancelScanButton.setTitleColor(colors.accentAlt, for: .normal)
        tableView.separatorColor = colors.stroke
        tableView.reloadData()
        // Shuffle's and repeat's active tint is the accent too.
        refreshTransport()
    }

    // MARK: - Transport

    @objc private func playPauseTapped() {
        engine.togglePlayPause()
    }

    @objc private func previousTapped() {
        engine.previous()
    }

    @objc private func nextTapped() {
        engine.next()
    }

    @objc private func toggleShuffle() {
        engine.shuffleEnabled.toggle()
        refreshTransport()
    }

    @objc private func cycleRepeat() {
        switch engine.repeatMode {
        case .off: engine.repeatMode = .all
        case .all: engine.repeatMode = .one
        case .one: engine.repeatMode = .off
        }
        refreshTransport()
    }

    @objc private func scrubStarted() {
        scrubbing = true
    }

    @objc private func scrubChanged() {
        elapsedLabel.text = timeString(TimeInterval(progressSlider.value))
    }

    @objc private func scrubEnded() {
        guard scrubbing else { return }
        scrubbing = false
        if engine.duration > 0 {
            engine.seek(to: TimeInterval(progressSlider.value))
        }
    }

    @objc private func volumeChanged() {
        engine.volume = volumeSlider.value
    }

    /// VOL on the wheel mutes. The slider below it is the fine control.
    @objc private func volumeTapped() {
        if engine.volume > 0.001 {
            volumeBeforeMute = engine.volume
            engine.volume = 0
        } else {
            engine.volume = volumeBeforeMute > 0.001 ? volumeBeforeMute : 1
        }
        volumeSlider.value = engine.volume
    }

    @objc private func cancelScanTapped() {
        library.cancelScans()
    }

    // MARK: - Menus

    /// Presenting from an alert action's handler races the alert's dismissal, and UIKit drops
    /// the new one, so it waits until the top controller is steady.
    private func presentModally(_ controller: UIViewController, attempt: Int = 0) {
        var top: UIViewController = workspaceHost ?? self
        while let presented = top.presentedViewController {
            top = presented
        }
        if (top.isBeingDismissed || top.isBeingPresented) && attempt < 8 {
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.15) { [weak self] in
                MainActor.assumeIsolated {
                    self?.presentModally(controller, attempt: attempt + 1)
                }
            }
            return
        }
        top.present(controller, animated: true)
    }

    private func presentSheet(title: String?, message: String? = nil, actions: [UIAlertAction], from source: UIView) {
        let sheet = UIAlertController(title: title, message: message, preferredStyle: .actionSheet)
        actions.forEach(sheet.addAction)
        sheet.addAction(UIAlertAction(title: "Cancel", style: .cancel))
        // An iPad presents this as a popover, which must have an anchor or UIKit raises.
        sheet.popoverPresentationController?.sourceView = source
        sheet.popoverPresentationController?.sourceRect = source.bounds
        presentModally(sheet)
    }

    private func promptForText(title: String, message: String?, placeholder: String, handler: @escaping @MainActor (String) -> Void) {
        let alert = UIAlertController(title: title, message: message, preferredStyle: .alert)
        alert.addTextField { field in
            field.placeholder = placeholder
            field.autocorrectionType = .no
            field.autocapitalizationType = .none
        }
        alert.addAction(UIAlertAction(title: "OK", style: .default) { [weak alert] _ in
            handler((alert?.textFields?.first?.text ?? "").trimmingCharacters(in: .whitespacesAndNewlines))
        })
        alert.addAction(UIAlertAction(title: "Cancel", style: .cancel))
        presentModally(alert)
    }

    /// In the status line as well as an alert, which is gone once it's been answered.
    private func showMessage(_ message: String) {
        engine.setStatus(message)
        let alert = UIAlertController(title: nil, message: message, preferredStyle: .alert)
        alert.addAction(UIAlertAction(title: "OK", style: .default))
        presentModally(alert)
    }

    @objc private func menuTapped() {
        presentSheet(title: "Music", actions: [
            UIAlertAction(title: "Add Music…", style: .default) { [weak self] _ in self?.addMusicTapped() },
            UIAlertAction(title: "Playlists…", style: .default) { [weak self] _ in self?.playlistsTapped() },
            UIAlertAction(title: "Equalizer…", style: .default) { [weak self] _ in
                self?.workspaceHost?.openOrFocusTool(WorkspaceToolRegistry.equalizer)
            },
        ], from: menuButton)
    }

    private func addMusicTapped() {
        presentSheet(title: "Add Music",
                     message: "Music lives in \(WorkspaceAudioLibrary.musicFolderDisplayName) by default, or add tracks from any path.",
                     actions: [
            UIAlertAction(title: "Play Music Folder", style: .default) { [weak self] _ in self?.loadMusicFolder(replacing: true) },
            UIAlertAction(title: "Add Music Folder to Queue", style: .default) { [weak self] _ in self?.loadMusicFolder(replacing: false) },
            UIAlertAction(title: "Add from Path…", style: .default) { [weak self] _ in self?.promptAddFromPath() },
        ], from: menuButton)
    }

    private func loadMusicFolder(replacing: Bool) {
        library.scanMusicFolder { [weak self] tracks in
            self?.applyScannedTracks(tracks, replacing: replacing, emptyMessage: "No audio files in \(WorkspaceAudioLibrary.musicFolderDisplayName).")
        }
    }

    private func promptAddFromPath() {
        promptForText(title: "Add from Path", message: "A folder or audio file path.", placeholder: WorkspaceAudioLibrary.musicFolderDisplayName) { [weak self] typed in
            guard !typed.isEmpty else { return }
            DispatchQueue.global(qos: .userInitiated).async {
                // Finding home takes the lock path expansion holds.
                let path = WorkspaceAudioLibrary.resolveTypedPath(typed)
                DispatchQueue.main.async {
                    MainActor.assumeIsolated {
                        _ = WorkspaceAudioLibrary.shared.scan(path: path) { [weak self] tracks in
                            self?.applyScannedTracks(tracks, replacing: false, emptyMessage: "No audio files found at that path.")
                        }
                    }
                }
            }
        }
    }

    private func applyScannedTracks(_ tracks: [WorkspaceAudioTrack]?, replacing: Bool, emptyMessage: String) {
        guard let tracks else {
            engine.setStatus("Scan cancelled.")
            return
        }
        guard !tracks.isEmpty else {
            showMessage(emptyMessage)
            return
        }
        if replacing || engine.queue.isEmpty {
            engine.setQueue(tracks, startIndex: 0)
        } else {
            engine.enqueue(tracks)
        }
        tableView.reloadData()
    }

    private func playlistsTapped() {
        library.playlistNames { [weak self] names in
            guard let self else { return }
            var actions = [
                UIAlertAction(title: "Save Queue as Playlist…", style: .default) { [weak self] _ in self?.promptSavePlaylist() },
            ]
            for name in names {
                actions.append(UIAlertAction(title: "Play “\(name)”", style: .default) { [weak self] _ in self?.loadPlaylist(name) })
            }
            if !names.isEmpty {
                actions.append(UIAlertAction(title: "Delete Playlist…", style: .destructive) { [weak self] _ in self?.promptDeletePlaylist(names) })
            }
            self.presentSheet(title: "Playlists", actions: actions, from: self.menuButton)
        }
    }

    private func promptSavePlaylist() {
        guard !engine.queue.isEmpty else {
            showMessage("The queue is empty.")
            return
        }
        promptForText(title: "Save Playlist", message: nil, placeholder: "Playlist name") { [weak self] name in
            guard let self, !name.isEmpty else { return }
            self.library.savePlaylist(named: name, tracks: self.engine.queue) { [weak self] saved in
                if !saved {
                    self?.showMessage("That playlist couldn’t be saved.")
                }
            }
        }
    }

    private func loadPlaylist(_ name: String) {
        library.tracks(forPlaylist: name) { [weak self] tracks in
            self?.applyScannedTracks(tracks, replacing: true, emptyMessage: "That playlist is empty.")
        }
    }

    private func promptDeletePlaylist(_ names: [String]) {
        let actions = names.map { name in
            UIAlertAction(title: name, style: .destructive) { [weak self] _ in self?.library.deletePlaylist(named: name) }
        }
        presentSheet(title: "Delete Playlist", actions: actions, from: menuButton)
    }

    // MARK: - Workspace

    /// Replaces the queue with this track, or the audio in this folder, and plays: that's what
    /// opening something means. File Manager's "Add Folder to Music" is for adding to the queue.
    func workspaceOpenFile(atPath path: String) {
        library.open(path: path)
    }

    // MARK: - Text scale

    override func applyTextScale() {
        super.applyTextScale()
        scaleLabel(statusLabel)
        tableView.rowHeight = trackRowHeight
        tableView.reloadData()
    }

    /// Not `scaledFontSize`, which rounds to half a point. Theme sizes are rarely on one, and
    /// the rounding would change the list at scale 1.
    private func trackListFontSize(_ style: UIFont.TextStyle) -> CGFloat {
        WorkspaceTheme.fontSize(style) * workspaceTextScale
    }

    // MARK: - The queue

    func tableView(_ tableView: UITableView, numberOfRowsInSection section: Int) -> Int {
        engine.queue.count
    }

    func tableView(_ tableView: UITableView, cellForRowAt indexPath: IndexPath) -> UITableViewCell {
        let cell = tableView.dequeueReusableCell(withIdentifier: Self.rowReuseIdentifier)
            ?? UITableViewCell(style: .subtitle, reuseIdentifier: Self.rowReuseIdentifier)
        let queue = engine.queue
        guard queue.indices.contains(indexPath.row) else { return cell }
        let track = queue[indexPath.row]
        let colors = theme
        let isCurrent = indexPath.row == engine.currentIndex
        cell.backgroundColor = .clear
        cell.textLabel?.text = track.title
        cell.textLabel?.lineBreakMode = .byTruncatingMiddle
        cell.textLabel?.textColor = isCurrent ? colors.accent : colors.primary
        cell.textLabel?.font = .systemFont(ofSize: trackListFontSize(.subheadline), weight: isCurrent ? .semibold : .regular)
        cell.detailTextLabel?.text = track.artist
        cell.detailTextLabel?.textColor = colors.secondary
        cell.detailTextLabel?.font = .systemFont(ofSize: trackListFontSize(.caption2))
        cell.accessibilityIdentifier = "workspace.music.row.\(indexPath.row)"
        return cell
    }

    func tableView(_ tableView: UITableView, didSelectRowAt indexPath: IndexPath) {
        tableView.deselectRow(at: indexPath, animated: true)
        engine.playTrack(at: indexPath.row)
    }

    func tableView(_ tableView: UITableView, editingStyleForRowAt indexPath: IndexPath) -> UITableViewCell.EditingStyle {
        .delete
    }

    func tableView(_ tableView: UITableView, commit editingStyle: UITableViewCell.EditingStyle, forRowAt indexPath: IndexPath) {
        if editingStyle == .delete {
            engine.removeTrack(at: indexPath.row)
        }
    }
}
