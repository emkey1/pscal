// Copyright 2026 Mike Miller
// SPDX-License-Identifier: Apache-2.0
//
// Ported by its author from iSH-AOK's Workspace (app/WorkspaceViewController.m at
// 0f75fee1c) and licensed to PSCAL under the Apache License 2.0.

import UIKit

/// What the status applets read from the system.
enum WorkspaceSystemInfo {
    /// A first line saying how many interfaces are up, then one line for each.
    static func networkSummaryText() -> String {
        var interfaces: UnsafeMutablePointer<ifaddrs>?
        guard getifaddrs(&interfaces) == 0, let first = interfaces else {
            return "Network: unavailable"
        }
        defer { freeifaddrs(interfaces) }

        var order: [String] = []
        var addresses: [String: (ipv4: String?, ipv6: String?)] = [:]
        var loopbackLine: String?
        var buffer = [CChar](repeating: 0, count: Int(INET6_ADDRSTRLEN))

        var cursor: UnsafeMutablePointer<ifaddrs>? = first
        while let current = cursor {
            defer { cursor = current.pointee.ifa_next }
            guard let address = current.pointee.ifa_addr else { continue }
            let family = Int32(address.pointee.sa_family)
            guard family == AF_INET || family == AF_INET6 else { continue }
            let flags = Int32(current.pointee.ifa_flags)
            guard flags & IFF_UP != 0 else { continue }

            let converted: UnsafePointer<CChar>?
            if family == AF_INET {
                converted = address.withMemoryRebound(to: sockaddr_in.self, capacity: 1) { pointer in
                    var value = pointer.pointee.sin_addr
                    return inet_ntop(family, &value, &buffer, socklen_t(buffer.count))
                }
            } else {
                converted = address.withMemoryRebound(to: sockaddr_in6.self, capacity: 1) { pointer in
                    var value = pointer.pointee.sin6_addr
                    return inet_ntop(family, &value, &buffer, socklen_t(buffer.count))
                }
            }
            guard converted != nil else { continue }
            let text = String(cString: buffer)
            // An IPv6 link-local address is noise; people want the IPv4 one.
            if family == AF_INET6 && text.hasPrefix("fe80") { continue }
            let name = String(cString: current.pointee.ifa_name)
            if flags & IFF_LOOPBACK != 0 {
                // The IPv4 loopback address rather than ::1.
                if loopbackLine == nil || family == AF_INET {
                    loopbackLine = "Loopback: \(name) (\(text))"
                }
                continue
            }
            if addresses[name] == nil {
                order.append(name)
            }
            var entry = addresses[name] ?? (nil, nil)
            if family == AF_INET {
                entry.ipv4 = entry.ipv4 ?? text
            } else {
                entry.ipv6 = entry.ipv6 ?? text
            }
            addresses[name] = entry
        }

        var lines: [String] = []
        var active = 0
        for name in order {
            let entry = addresses[name]
            guard let address = entry?.ipv4 ?? entry?.ipv6 else { continue }
            lines.append("\(name): \(entry?.ipv4 != nil ? "IPv4" : "IPv6")  \(address)")
            active += 1
            if active >= 3 { break }
        }
        if let loopbackLine {
            lines.append(loopbackLine)
        }
        if active == 0 && loopbackLine == nil {
            return "Network: no active interfaces"
        }
        return "Active interfaces: \(active)\n" + lines.joined(separator: "\n")
    }

    static func primaryNetworkLine() -> String {
        let lines = networkSummaryText().components(separatedBy: "\n")
        return lines.count >= 2 ? lines[1] : (lines.first ?? "Network: unavailable")
    }

    static func durationString(_ interval: TimeInterval) -> String {
        let total = max(0, Int(interval.rounded()))
        let days = total / 86400
        let hours = (total % 86400) / 3600
        let minutes = (total % 3600) / 60
        let seconds = total % 60
        if days > 0 {
            return String(format: "%dd %02dh %02dm", days, hours, minutes)
        }
        if hours > 0 {
            return String(format: "%dh %02dm %02ds", hours, minutes, seconds)
        }
        return String(format: "%dm %02ds", minutes, seconds)
    }

    /// What this app's using.
    static func memoryFootprint() -> UInt64? {
        var info = task_vm_info_data_t()
        var count = mach_msg_type_number_t(MemoryLayout<task_vm_info_data_t>.size / MemoryLayout<natural_t>.size)
        let result = withUnsafeMutablePointer(to: &info) { pointer in
            pointer.withMemoryRebound(to: integer_t.self, capacity: Int(count)) {
                task_info(mach_task_self_, task_flavor_t(TASK_VM_INFO), $0, &count)
            }
        }
        return result == KERN_SUCCESS ? info.phys_footprint : nil
    }
}

/// The time, the date and the zone, sized to fit the window.
@MainActor
final class WorkspaceClockAppletViewController: WorkspaceAppletViewController {
    private let timeFormatter = DateFormatter()
    private let dateFormatter = DateFormatter()
    private var heroCard: UIView!
    private var stack: UIStackView!
    private var timeLabel: UILabel!
    private var dateLabel: UILabel!
    private var zoneLabel: UILabel!
    private var timer: Timer?

    override func viewDidLoad() {
        super.viewDidLoad()
        timeFormatter.timeStyle = .medium
        timeFormatter.dateStyle = .none
        dateFormatter.dateStyle = .full
        dateFormatter.timeStyle = .none

        let card = stackCard(spacing: 8, inset: 0)
        heroCard = card.card
        stack = card.stack
        stack.alignment = .center
        toolContentView.addSubview(heroCard)

        timeLabel = themeAccentLabel(style: .largeTitle, monospaced: true)
        timeLabel.numberOfLines = 1
        timeLabel.adjustsFontSizeToFitWidth = true
        timeLabel.minimumScaleFactor = 0.5
        dateLabel = themePrimaryLabel(style: .title3)
        dateLabel.textAlignment = .center
        zoneLabel = themeSecondaryLabel(style: .footnote)
        zoneLabel.textAlignment = .center
        stack.addArrangedSubview(timeLabel)
        stack.addArrangedSubview(dateLabel)
        stack.addArrangedSubview(zoneLabel)
        stack.isLayoutMarginsRelativeArrangement = true
        stack.directionalLayoutMargins = NSDirectionalEdgeInsets(top: 8, leading: 10, bottom: 8, trailing: 10)

        NSLayoutConstraint.activate([
            heroCard.centerXAnchor.constraint(equalTo: toolContentView.centerXAnchor),
            heroCard.centerYAnchor.constraint(equalTo: toolContentView.centerYAnchor),
            heroCard.leadingAnchor.constraint(greaterThanOrEqualTo: toolContentView.leadingAnchor, constant: 10),
            heroCard.trailingAnchor.constraint(lessThanOrEqualTo: toolContentView.trailingAnchor, constant: -10),
            heroCard.widthAnchor.constraint(lessThanOrEqualToConstant: 280),
        ])
        applyTheme()
        refreshClock()
    }

    override func viewDidLayoutSubviews() {
        super.viewDidLayoutSubviews()
        let insetY = WorkspaceTheme.densityValue(4, 12)
        let insetX = WorkspaceTheme.densityValue(6, 16)
        let bounds = toolContentView.bounds.insetBy(dx: insetX, dy: insetY)
        let width = max(1, bounds.width)
        let height = max(1, bounds.height)
        let timeSize = min(max(min(width * 0.22, height * 0.42), 18), 42)
        let dateSize = min(max(min(width * 0.055, height * 0.14), 9), 14)
        timeLabel.font = .monospacedDigitSystemFont(ofSize: timeSize, weight: .bold)
        dateLabel.font = .systemFont(ofSize: dateSize, weight: .semibold)
        zoneLabel.font = .systemFont(ofSize: max(9, (dateSize * 0.76).rounded()), weight: .medium)
        stack.spacing = max(2, (timeSize * 0.08).rounded())
    }

    override func viewWillAppear(_ animated: Bool) {
        super.viewWillAppear(animated)
        timer?.invalidate()
        timer = Timer.scheduledTimer(timeInterval: 1, target: self, selector: #selector(refreshClock), userInfo: nil, repeats: true)
        refreshClock()
    }

    override func viewDidDisappear(_ animated: Bool) {
        super.viewDidDisappear(animated)
        timer?.invalidate()
        timer = nil
    }

    @objc private func refreshClock() {
        let now = Date()
        timeLabel.text = timeFormatter.string(from: now)
        dateLabel.text = dateFormatter.string(from: now)
        let zone = TimeZone.current
        zoneLabel.text = "\(zone.abbreviation() ?? "Local")  •  \(zone.identifier)"
    }

    override func applyTheme() {
        super.applyTheme()
        guard let heroCard else { return }
        let colors = theme
        // Effectively opaque. At 0.88 the background bled through and dragged the readout's
        // contrast below what the theme guarantees on darker palettes.
        heroCard.backgroundColor = colors.card.withAlphaComponent(0.98)
        // The focal reading, and it shrinks in small windows, so it holds a firmer floor.
        timeLabel.textColor = WorkspaceTheme.adjusted(colors.accent, against: colors.card, minimumContrast: 5.5)
        dateLabel.textColor = WorkspaceTheme.adjusted(colors.primary, against: colors.card, minimumContrast: 7)
        zoneLabel.textColor = WorkspaceTheme.adjusted(colors.accentAlt, against: colors.card, minimumContrast: 4.6)
    }
}

/// CPU, memory, battery and storage gauges over a card of details.
@MainActor
final class WorkspaceMonitorAppletViewController: WorkspaceAppletViewController, WorkspaceTextScalable {
    private var contentStack: UIStackView!
    private var gaugeRows: [UIStackView] = []
    private var cpu: (subtitle: UILabel, gauge: WorkspaceGaugeView)!
    private var memory: (subtitle: UILabel, gauge: WorkspaceGaugeView)!
    private var battery: (subtitle: UILabel, gauge: WorkspaceGaugeView)!
    private var storage: (subtitle: UILabel, gauge: WorkspaceGaugeView)!
    private var uptimeValue: UILabel!
    private var networkValue: UILabel!
    private var startupValue: UILabel!
    private var liveValue: UILabel!
    private var detailRowLabels: [UILabel] = []
    private var timer: Timer?
    private var previousTicks: [UInt32]?
    private var appliedTextScale: CGFloat = 1
    private var gaugeObserver: NSObjectProtocol?

    /// The window's frame for the gauge style in use: rings are much taller than bars, so a
    /// window sized for bars clips the bottom row of dials, and one sized for rings leaves
    /// dead space under bars.
    static func windowSize(textScale: CGFloat) -> CGSize {
        let phone = WorkspaceMetrics.usesPhoneLayout
        let rings = WorkspaceTheme.usesRingGauges
        let width: CGFloat = phone ? 328 : 360
        let gaugeHeight: CGFloat = rings ? (phone ? 42 : 54) : (phone ? 20 : 28)
        let tileInset: CGFloat = phone ? 5 : 7
        let tileHeight = max(phone ? 50 : 64, tileInset * 2 + 20 + 5 + gaugeHeight)
        // Four rows at 16 points, with the card's insets and some slack.
        let detailsHeight: CGFloat = phone ? 94 : 106
        let contentHeight = 6 * 2 + tileHeight * 2 + 8 * 2 + detailsHeight
        var size = CGSize(width: width, height: contentHeight + WorkspaceMetrics.titleBarHeight)
        guard abs(textScale - 1) >= 0.001 else { return size }

        func lineHeight(_ pointSize: CGFloat, _ scale: CGFloat, _ weight: UIFont.Weight) -> CGFloat {
            let scaled = (pointSize * scale * 2).rounded() / 2
            return UIFont.systemFont(ofSize: scaled, weight: weight).lineHeight.rounded(.up)
        }
        // Only the readings scale, so only their rows add height. Each row is as tall as its
        // icon until the text outgrows it.
        let rowGrowth = max(16, lineHeight(12, textScale, .semibold)) - max(16, lineHeight(12, 1, .semibold))
        let headerGrowth = max(20, lineHeight(12, textScale, .regular)) - max(20, lineHeight(12, 1, .regular))
        size.height += 4 * max(0, rowGrowth) + 2 * max(0, headerGrowth)
        // Across, the text widens and the insets, icon and gaps beside it don't. Left at the
        // default width, fit-to-width would shrink the larger text straight back.
        let chrome: CGFloat = 6 * 2 + 12 * 2 + 18 + 10 * 2
        size.width = max(width, (chrome + (width - chrome) * textScale).rounded(.up))
        return size
    }

    deinit {
        if let gaugeObserver {
            NotificationCenter.default.removeObserver(gaugeObserver)
        }
    }

    override func viewDidLoad() {
        super.viewDidLoad()
        contentStack = installScrollingStack(spacing: 8, inset: 6).stack

        func addRow(_ tiles: [UIView]) {
            let row = UIStackView()
            row.axis = .horizontal
            row.spacing = 8
            row.distribution = .fillEqually
            tiles.forEach(row.addArrangedSubview)
            contentStack.addArrangedSubview(row)
            gaugeRows.append(row)
        }
        let cpuTile = gaugeTile(icon: "cpu", caption: "CPU")
        let memoryTile = gaugeTile(icon: "memorychip", caption: "Memory")
        let batteryTile = gaugeTile(icon: "battery.100", caption: "Battery")
        let storageTile = gaugeTile(icon: "internaldrive", caption: "Storage")
        cpu = (cpuTile.subtitle, cpuTile.gauge)
        memory = (memoryTile.subtitle, memoryTile.gauge)
        battery = (batteryTile.subtitle, batteryTile.gauge)
        storage = (storageTile.subtitle, storageTile.gauge)
        addRow([cpuTile.card, memoryTile.card])
        addRow([batteryTile.card, storageTile.card])

        let uptime = iconRow(icon: "clock", title: "Uptime")
        let network = iconRow(icon: "network", title: "Network")
        let startup = iconRow(icon: "bolt", title: "Startup")
        let live = iconRow(icon: "square.grid.2x2", title: "Live")
        uptimeValue = uptime.value
        networkValue = network.value
        startupValue = startup.value
        liveValue = live.value
        contentStack.addArrangedSubview(rowsCard(rows: [uptime.row, network.row, startup.row, live.row]))
        detailRowLabels = [uptime.title, uptime.value, network.title, network.value,
                           startup.title, startup.value, live.title, live.value]

        gaugeObserver = NotificationCenter.default.addObserver(forName: .workspaceGaugeStyleDidChange, object: nil, queue: .main) { [weak self] _ in
            MainActor.assumeIsolated {
                self?.gaugeStyleDidChange()
            }
        }
        refreshMonitor()
    }

    /// Next turn, so the gauges lay themselves out again first, and at this window's text
    /// size, or a larger details card is clipped.
    private func gaugeStyleDidChange() {
        DispatchQueue.main.async { [weak self] in
            MainActor.assumeIsolated {
                guard let self, let host = self.workspaceHost, let window = host.window(holding: self) else { return }
                host.resizeWindow(window, to: Self.windowSize(textScale: self.workspaceTextScale))
                self.noteAutosizedWindow()
            }
        }
    }

    override func viewWillAppear(_ animated: Bool) {
        super.viewWillAppear(animated)
        UIDevice.current.isBatteryMonitoringEnabled = true
        timer?.invalidate()
        timer = Timer.scheduledTimer(timeInterval: 1, target: self, selector: #selector(refreshMonitor), userInfo: nil, repeats: true)
        refreshMonitor()
    }

    override func viewDidDisappear(_ animated: Bool) {
        super.viewDidDisappear(animated)
        timer?.invalidate()
        timer = nil
    }

    override func viewDidLayoutSubviews() {
        super.viewDidLayoutSubviews()
        let compactWidth = toolContentView.bounds.width < 240
        for row in gaugeRows {
            row.axis = compactWidth ? .vertical : .horizontal
            row.distribution = compactWidth ? .fill : .fillEqually
        }
        contentStack.spacing = WorkspaceTheme.densityValue(4, 8)
    }

    private func sampleCPURatio() -> Double? {
        var info = host_cpu_load_info_data_t()
        var count = mach_msg_type_number_t(MemoryLayout<host_cpu_load_info_data_t>.size / MemoryLayout<integer_t>.size)
        let port = mach_host_self()
        let result = withUnsafeMutablePointer(to: &info) { pointer in
            pointer.withMemoryRebound(to: integer_t.self, capacity: Int(count)) {
                host_statistics(port, HOST_CPU_LOAD_INFO, $0, &count)
            }
        }
        mach_port_deallocate(mach_task_self_, port)
        guard result == KERN_SUCCESS else { return nil }
        // User, system, idle, nice.
        let ticks = [info.cpu_ticks.0, info.cpu_ticks.1, info.cpu_ticks.2, info.cpu_ticks.3]
        var total: UInt64 = 0
        var active: UInt64 = 0
        for (index, current) in ticks.enumerated() {
            let delta = UInt64(previousTicks.map { current &- $0[index] } ?? current)
            total += delta
            if index != 2 {
                active += delta
            }
        }
        previousTicks = ticks
        return total == 0 ? 0 : Double(active) / Double(total)
    }

    private func setGauge(_ tile: (subtitle: UILabel, gauge: WorkspaceGaugeView), ratio: Double?, subtitle: String) {
        tile.gauge.progress = ratio ?? 0
        tile.gauge.valueText = ratio.map { "\(Int(($0 * 100).rounded()))%" } ?? "--"
        tile.subtitle.text = ratio == nil ? "n/a" : subtitle
    }

    @objc private func refreshMonitor() {
        setGauge(cpu, ratio: sampleCPURatio(), subtitle: "load")

        let physical = ProcessInfo.processInfo.physicalMemory
        let footprint = WorkspaceSystemInfo.memoryFootprint()
        let memoryRatio = (footprint != nil && physical > 0) ? Double(footprint ?? 0) / Double(physical) : nil
        setGauge(memory, ratio: memoryRatio, subtitle: "in use")

        let device = UIDevice.current
        if device.batteryState == .unknown || device.batteryLevel < 0 {
            setGauge(battery, ratio: nil, subtitle: "")
        } else {
            let state: String
            switch device.batteryState {
            case .charging: state = "charging"
            case .full: state = "full"
            default: state = "on battery"
            }
            setGauge(battery, ratio: max(0, min(1, Double(device.batteryLevel))), subtitle: state)
        }

        let attributes = try? FileManager.default.attributesOfFileSystem(forPath: NSHomeDirectory())
        if let free = attributes?[.systemFreeSize] as? NSNumber, let size = attributes?[.systemSize] as? NSNumber, size.int64Value > 0 {
            let used = Double(size.int64Value - free.int64Value) / Double(size.int64Value)
            let freeText = ByteCountFormatter.string(fromByteCount: free.int64Value, countStyle: .file)
            setGauge(storage, ratio: max(0, min(1, used)), subtitle: "\(freeText) free")
        } else {
            setGauge(storage, ratio: nil, subtitle: "")
        }

        uptimeValue.text = WorkspaceSystemInfo.durationString(ProcessInfo.processInfo.systemUptime)
        networkValue.text = WorkspaceSystemInfo.primaryNetworkLine()
        startupValue.text = WorkspaceSettings.opensAtLaunch ? "Workspace" : "Tabs"
        let sessions = TerminalTabManager.shared.tabs.count
        liveValue.text = sessions == 1 ? "1 session" : "\(sessions) sessions"
    }

    override func applyTextScale() {
        super.applyTextScale()
        guard contentStack != nil else { return }
        [cpu.subtitle, memory.subtitle, battery.subtitle, storage.subtitle].forEach(scaleLabel)
        detailRowLabels.forEach(scaleLabel)
        let previous = appliedTextScale
        appliedTextScale = workspaceTextScale
        guard abs(appliedTextScale - previous) >= 0.001 else { return }
        growWindowForTextScale(from: Self.windowSize(textScale: previous), to: Self.windowSize(textScale: appliedTextScale))
    }

    override func applyTheme() {
        super.applyTheme()
        guard contentStack != nil else { return }
        memory.gauge.fillColor = theme.accentAlt
        storage.gauge.fillColor = theme.accentAlt
    }
}

/// The interfaces that are up, with their addresses.
@MainActor
final class WorkspaceNetworksAppletViewController: WorkspaceAppletViewController, WorkspaceTextScalable {
    private var contentStack: UIStackView!
    private var summaryLabel: UILabel!
    private var textView: UITextView!
    private var detailsMinimumHeight: NSLayoutConstraint!
    private var timer: Timer?

    /// The scrolling text view gives the card no height, so this is its height. It follows
    /// the text size from 1 up.
    private var detailsHeight: CGFloat {
        (WorkspaceMetrics.usesPhoneLayout ? 92 : 110) * max(1, workspaceTextScale)
    }

    override func viewDidLoad() {
        super.viewDidLoad()
        contentStack = installScrollingStack(spacing: 8, inset: 6).stack

        let summary = stackCard(spacing: 6, inset: 12)
        let eyebrow = themeSecondaryLabel(style: .caption1)
        eyebrow.text = "CONNECTIVITY"
        eyebrow.font = .systemFont(ofSize: 9, weight: .semibold)
        summaryLabel = themeAccentLabel(style: .headline)
        summary.stack.addArrangedSubview(eyebrow)
        summary.stack.addArrangedSubview(summaryLabel)

        let details = themeCardView()
        textView = themeTextView()
        details.addSubview(textView)
        detailsMinimumHeight = details.heightAnchor.constraint(greaterThanOrEqualToConstant: detailsHeight)
        NSLayoutConstraint.activate([
            detailsMinimumHeight,
            textView.topAnchor.constraint(equalTo: details.topAnchor, constant: 8),
            textView.leadingAnchor.constraint(equalTo: details.leadingAnchor, constant: 8),
            textView.trailingAnchor.constraint(equalTo: details.trailingAnchor, constant: -8),
            textView.bottomAnchor.constraint(equalTo: details.bottomAnchor, constant: -8),
        ])
        contentStack.addArrangedSubview(summary.card)
        contentStack.addArrangedSubview(details)
        refreshNetworks()
    }

    override func viewWillAppear(_ animated: Bool) {
        super.viewWillAppear(animated)
        timer?.invalidate()
        timer = Timer.scheduledTimer(timeInterval: 2, target: self, selector: #selector(refreshNetworks), userInfo: nil, repeats: true)
        refreshNetworks()
    }

    override func viewDidDisappear(_ animated: Bool) {
        super.viewDidDisappear(animated)
        timer?.invalidate()
        timer = nil
    }

    override func viewDidLayoutSubviews() {
        super.viewDidLayoutSubviews()
        contentStack.spacing = WorkspaceTheme.densityValue(4, 8)
    }

    @objc private func refreshNetworks() {
        let lines = WorkspaceSystemInfo.networkSummaryText().components(separatedBy: "\n")
        summaryLabel.text = lines.first ?? "Network unavailable"
        textView.text = lines.count > 1 ? lines.dropFirst().joined(separator: "\n") : "No active interfaces to display."
    }

    override func applyTheme() {
        super.applyTheme()
        summaryLabel?.textColor = theme.accentAlt
    }

    override func applyTextScale() {
        super.applyTextScale()
        guard contentStack != nil else { return }
        scaleLabel(summaryLabel)
        detailsMinimumHeight.constant = detailsHeight
    }
}

/// The end of the app's runtime log.
@MainActor
final class WorkspaceLogsAppletViewController: WorkspaceAppletViewController, WorkspaceTextScalable {
    private nonisolated static let tailBytes: UInt64 = 128 * 1024
    private nonisolated static let tailLines = 200

    private var sourceLabel: UILabel!
    private var logTextView: UITextView!
    private var timer: Timer?
    private var generation = 0
    private var shownText = ""

    override func viewDidLoad() {
        super.viewDidLoad()
        let card = themeCardView()
        toolContentView.addSubview(card)
        let stack = UIStackView()
        stack.translatesAutoresizingMaskIntoConstraints = false
        stack.axis = .vertical
        stack.spacing = 6
        card.addSubview(stack)

        let header = tileHeaderRow(icon: "doc.text", caption: "RECENT LOGS", trailing: true)
        sourceLabel = header.trailing
        stack.addArrangedSubview(header.row)

        // Fills the window under the fixed header and scrolls itself, so resizing the window
        // changes how much of the log shows.
        logTextView = themeTextView()
        logTextView.isScrollEnabled = true
        logTextView.setContentHuggingPriority(.defaultLow, for: .vertical)
        logTextView.setContentCompressionResistancePriority(.defaultLow, for: .vertical)
        stack.addArrangedSubview(logTextView)

        NSLayoutConstraint.activate([
            card.topAnchor.constraint(equalTo: toolContentView.topAnchor, constant: 6),
            card.leadingAnchor.constraint(equalTo: toolContentView.leadingAnchor, constant: 6),
            card.trailingAnchor.constraint(equalTo: toolContentView.trailingAnchor, constant: -6),
            card.bottomAnchor.constraint(equalTo: toolContentView.bottomAnchor, constant: -6),
            stack.topAnchor.constraint(equalTo: card.topAnchor, constant: 10),
            stack.leadingAnchor.constraint(equalTo: card.leadingAnchor, constant: 12),
            stack.trailingAnchor.constraint(equalTo: card.trailingAnchor, constant: -12),
            stack.bottomAnchor.constraint(equalTo: card.bottomAnchor, constant: -10),
        ])
        sourceLabel.text = "reading…"
        logTextView.text = "Loading recent log entries…"
    }

    override func viewWillAppear(_ animated: Bool) {
        super.viewWillAppear(animated)
        refreshLogs()
        timer?.invalidate()
        timer = Timer.scheduledTimer(timeInterval: 5, target: self, selector: #selector(refreshLogs), userInfo: nil, repeats: true)
    }

    override func viewDidDisappear(_ animated: Bool) {
        super.viewDidDisappear(animated)
        timer?.invalidate()
        timer = nil
    }

    /// Off the main thread: the log can be large, and only its end is wanted.
    @objc private func refreshLogs() {
        generation += 1
        let current = generation
        let url = RuntimeLogger.runtime.logFileURL
        DispatchQueue.global(qos: .utility).async { [weak self] in
            let text = Self.readTail(of: url)
            DispatchQueue.main.async {
                MainActor.assumeIsolated {
                    guard let self, current == self.generation else { return }
                    self.show(text, source: url.lastPathComponent)
                }
            }
        }
    }

    private nonisolated static func readTail(of url: URL) -> String? {
        guard let handle = try? FileHandle(forReadingFrom: url) else { return nil }
        defer { try? handle.close() }
        guard let size = try? handle.seekToEnd() else { return nil }
        let start = size > tailBytes ? size - tailBytes : 0
        guard (try? handle.seek(toOffset: start)) != nil, let data = try? handle.readToEnd() else { return nil }
        var lines = String(decoding: data, as: UTF8.self).components(separatedBy: "\n")
        // A read from the middle of the file starts in the middle of a line.
        if start > 0 && !lines.isEmpty {
            lines.removeFirst()
        }
        return lines.suffix(tailLines).joined(separator: "\n")
    }

    private func show(_ text: String?, source: String) {
        sourceLabel.text = source
        let trimmed = text?.trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
        let next = trimmed.isEmpty ? "The runtime log is empty so far." : trimmed
        guard next != shownText else { return }
        // Stay at the end if that's where the person was; don't pull them off a line they
        // scrolled back to read.
        let bounds = logTextView.bounds
        let wasAtEnd = shownText.isEmpty || logTextView.contentOffset.y + bounds.height >= logTextView.contentSize.height - 24
        let offset = logTextView.contentOffset
        shownText = next
        logTextView.text = next
        if wasAtEnd {
            logTextView.scrollRangeToVisible(NSRange(location: (next as NSString).length, length: 0))
        } else {
            logTextView.contentOffset = offset
        }
    }
}
