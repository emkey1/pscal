// Copyright 2026 Mike Miller
// SPDX-License-Identifier: Apache-2.0
//
// The native half of the iOS terminal. hterm draws inside a web view (the page is in
// TerminalWeb/). Keyboard input and pastes stay out of the page: a hidden native text view
// takes them and hands them to the runtime. Scrolling is split too: a native scroll view
// shows the indicator and takes the drag, and tells the page where to scroll.

import Foundation
import SwiftUI
import UIKit
import WebKit

private struct TerminalGrid: Equatable {
    let columns: Int
    let rows: Int

    init(columns: Int, rows: Int) {
        self.columns = columns
        self.rows = rows
    }

    /// Reads what the page reports, `{columns, rows}` or `[columns, rows]`. A grid with an
    /// empty side counts as no answer.
    init?(pageValue: Any?) {
        let first: NSNumber?
        let second: NSNumber?
        if let object = pageValue as? [String: Any] {
            first = object["columns"] as? NSNumber
            second = object["rows"] as? NSNumber
        } else if let pair = pageValue as? [NSNumber], pair.count == 2 {
            first = pair[0]
            second = pair[1]
        } else {
            return nil
        }
        guard let columns = first?.intValue, let rows = second?.intValue, columns > 0, rows > 0 else {
            return nil
        }
        self.init(columns: columns, rows: rows)
    }
}

/// Output on its way to the page, oldest first. Kept as a list of the chunks it arrived in so
/// that taking from the front never moves what's behind it.
private struct OutputBacklog {
    private var chunks: [Data] = []
    private var head = 0
    private(set) var byteCount = 0

    var isEmpty: Bool { byteCount == 0 }

    mutating func append(_ data: Data) {
        guard !data.isEmpty else { return }
        chunks.append(data)
        byteCount += data.count
    }

    /// Drops the oldest bytes until no more than `limit` remain. Returns whether any went.
    mutating func trim(toAtMost limit: Int) -> Bool {
        guard byteCount > limit else { return false }
        var excess = byteCount - limit
        byteCount = limit
        while excess > 0 {
            let oldest = chunks[head]
            if oldest.count <= excess {
                excess -= oldest.count
                chunks[head] = Data()
                head += 1
            } else {
                chunks[head] = oldest.dropFirst(excess)
                excess = 0
            }
        }
        compact()
        return true
    }

    mutating func take(upTo maxBytes: Int) -> Data {
        var taken = Data()
        taken.reserveCapacity(min(maxBytes, byteCount))
        while taken.count < maxBytes, head < chunks.count {
            let room = maxBytes - taken.count
            let oldest = chunks[head]
            if oldest.count <= room {
                taken.append(oldest)
                chunks[head] = Data()
                head += 1
            } else {
                taken.append(oldest.prefix(room))
                chunks[head] = oldest.dropFirst(room)
            }
        }
        byteCount -= taken.count
        compact()
        return taken
    }

    mutating func removeAll() {
        chunks.removeAll(keepingCapacity: true)
        head = 0
        byteCount = 0
    }

    private mutating func compact() {
        if head == chunks.count {
            removeAll()
        } else if head > 64 && head > chunks.count / 2 {
            chunks.removeFirst(head)
            head = 0
        }
    }
}

/// One terminal page in one web view, for as long as the controller lives.
///
/// Not main-actor isolated, because sessions call it from their output queues. Everything
/// that isn't marked otherwise belongs to the main thread.
final class HtermTerminalController: NSObject {
    struct OutputMetrics {
        let pendingBytes: Int
        let isLoaded: Bool
        let hostSizeReady: Bool
        let trimCount: Int
        let reloadCount: Int
        let discardCount: Int
    }

    /// Raised by the App Diagnostics button. It's the one memory signal a tester can trigger
    /// in the simulator.
    static let releaseScrollbackRequest = Notification.Name("com.pscal.terminal.releaseScrollbackRequest")

    static let debugEnabled: Bool = {
        let environment = ProcessInfo.processInfo.environment
        let value = environment["PSCALI_HERM_DEBUG"] ?? environment["PSCALI_PTY_OUTPUT_LOG"]
        return value.map { $0 != "0" } ?? false
    }()

    fileprivate static let pageMessageNames = [
        "load", "syncFocus", "focus", "sendInput", "resize", "propUpdate",
        "newScrollHeight", "newScrollTop", "openLink", "selectionChanged", "log",
    ]

    private static let maxBacklogBytes = 2 * 1024 * 1024
    private static let maxChunkBytes = 128 * 1024
    private static let reloadDelay: TimeInterval = 1.15
    private static let styleSettleDelay: TimeInterval = 0.06
    private static var lastInstanceId = 0

    let instanceId: Int
    let webView: WKWebView
    var onLoaded: (() -> Void)?

    var isLoaded: Bool { readShared().isLoaded }

    /// The few facts other threads ask about. Metrics and an attaching runtime read them from
    /// their own queues, and those queues can have the main thread waiting on them, so
    /// reading must never wait for the main thread in turn.
    private struct SharedState {
        var pendingBytes = 0
        var isLoaded = false
        var hostSizeReady = false
        var trimCount = 0
        var reloadCount = 0
        var discardCount = 0
    }

    private let sharedLock = NSLock()
    private var shared = SharedState()

    private let terminalWebView: TerminalWebView
    private let userContent: WKUserContentController
    private var memorySource: DispatchSourceMemoryPressure?

    /// The container showing this terminal right now. Scroll, selection, focus and cursor key
    /// messages go here and nowhere else.
    private weak var displayTarget: HtermTerminalContainerView?
    /// The container holding the SwiftUI callbacks. Those closures capture the session, which
    /// owns this controller, so holding them here would keep the two alive for ever.
    private weak var callbackHost: HtermTerminalContainerView?

    private var backlog = OutputBacklog()
    private var epoch = 1
    private var writeInFlight: Int?
    private var lastWriteId = 0
    private var outputHeldLogged = false
    private var trimLogged = false
    private var firstOutputLogged = false
    private var firstChunkLogged = false

    private var lastHostSize: CGSize?
    private var pageGrid: TerminalGrid?
    private var forcedGrid: TerminalGrid?
    private var runtimeGrid: TerminalGrid?
    private var gridAwaitingSession: TerminalGrid?
    private var resizeSessionId: UInt64 = 0
    private var lastGridQuery = 0

    private var styleScript: String?
    private var wantsPageFocus = false
    private var scrollToBottomQueued = false
    private var reloadQueued = false

    /// Returns a new controller from any thread. A caller off the main thread waits for it.
    static func makeOnMainThread() -> HtermTerminalController {
        if Thread.isMainThread {
            return HtermTerminalController()
        }
        return DispatchQueue.main.sync { HtermTerminalController() }
    }

    private override init() {
        Self.lastInstanceId += 1
        instanceId = Self.lastInstanceId

        let relay = PageMessageRelay()
        let content = WKUserContentController()
        let debugValue = Self.debugEnabled ? "true" : "false"
        let globals = "window.PSCALI_HERM_DEBUG = \(debugValue); window.PSCALI_HERM_INSTANCE_ID = \(instanceId);"
        content.addUserScript(WKUserScript(source: globals, injectionTime: .atDocumentStart, forMainFrameOnly: true))
        Self.pageMessageNames.forEach { content.add(relay, name: $0) }
        userContent = content

        let configuration = WKWebViewConfiguration()
        configuration.userContentController = content
        configuration.setURLSchemeHandler(TerminalAssetSchemeHandler.shared, forURLScheme: TerminalAssetSchemeHandler.scheme)
        // The grid is enormous until a container sizes this, and that's expected: output
        // stays held until then.
        let web = TerminalWebView(frame: CGRect(x: 0, y: 0, width: 10_000, height: 10_000), configuration: configuration)
        terminalWebView = web
        webView = web

        super.init()

        relay.controller = self
        applyWebViewSettings()
        web.navigationDelegate = self
        startMemorySignals()
        tabInitLog("Hterm[\(instanceId)] created")
        loadPage()
    }

    deinit {
        memorySource?.cancel()
        NotificationCenter.default.removeObserver(self)
        let content = userContent
        let web = webView
        // The last reference can be dropped on a session's queue, and neither of these may be
        // touched there. Capturing the web view also makes sure it's released on main.
        let unregister = {
            Self.pageMessageNames.forEach { content.removeScriptMessageHandler(forName: $0) }
            withExtendedLifetime(web) {}
        }
        if Thread.isMainThread {
            unregister()
        } else {
            DispatchQueue.main.async(execute: unregister)
        }
    }

    func attach(to view: UIView) {
        webView.removeFromSuperview()
        webView.translatesAutoresizingMaskIntoConstraints = false
        view.addSubview(webView)
        NSLayoutConstraint.activate([
            webView.topAnchor.constraint(equalTo: view.topAnchor),
            webView.leadingAnchor.constraint(equalTo: view.leadingAnchor),
            webView.trailingAnchor.constraint(equalTo: view.trailingAnchor),
            webView.bottomAnchor.constraint(equalTo: view.bottomAnchor),
        ])
    }

    // MARK: Shared state

    private func readShared() -> SharedState {
        sharedLock.lock()
        defer { sharedLock.unlock() }
        return shared
    }

    private func updateShared(_ change: (inout SharedState) -> Void) {
        sharedLock.lock()
        defer { sharedLock.unlock() }
        change(&shared)
    }

    func outputMetrics() -> OutputMetrics {
        let state = readShared()
        return OutputMetrics(pendingBytes: state.pendingBytes,
                             isLoaded: state.isLoaded,
                             hostSizeReady: state.hostSizeReady,
                             trimCount: state.trimCount,
                             reloadCount: state.reloadCount,
                             discardCount: state.discardCount)
    }

    // MARK: Output

    /// Output, snapshots and discards all reach the main thread this way, and sessions depend
    /// on them staying in order. A detach schedules its discard on main and the next attach
    /// sends a snapshot straight after: if the snapshot jumped that queue the discard would
    /// wipe it, and output that followed the snapshot would be overwritten by it.
    private func onMain(_ work: @escaping () -> Void) {
        if Thread.isMainThread {
            work()
        } else {
            DispatchQueue.main.async(execute: work)
        }
    }

    func enqueueOutput(_ data: Data) {
        guard !data.isEmpty else { return }
        onMain { [weak self] in
            self?.addToBacklog(data)
        }
    }

    func reloadFromSnapshot(_ data: Data) {
        onMain { [weak self] in
            guard let self else { return }
            self.epoch += 1
            self.backlog.removeAll()
            self.backlog.append(data)
            let pending = self.backlog.byteCount
            self.updateShared {
                $0.pendingBytes = pending
                $0.reloadCount += 1
            }
            // A page that isn't up yet gets this epoch when it says it has loaded.
            if self.isLoaded {
                self.callPage("exports.resetForEpoch(\(self.epoch))")
            }
            self.deliverOutput()
        }
    }

    func discardPendingOutput() {
        onMain { [weak self] in
            guard let self else { return }
            self.epoch += 1
            self.backlog.removeAll()
            self.updateShared {
                $0.pendingBytes = 0
                $0.discardCount += 1
            }
        }
    }

    private func addToBacklog(_ data: Data) {
        if Self.debugEnabled && !firstOutputLogged {
            firstOutputLogged = true
            NSLog("%@", "Hterm[\(instanceId)] first output queued, \(data.count) bytes")
        }
        backlog.append(data)
        let trimmed = backlog.trim(toAtMost: Self.maxBacklogBytes)
        let pending = backlog.byteCount
        updateShared {
            $0.pendingBytes = pending
            if trimmed {
                $0.trimCount += 1
            }
        }
        if trimmed && !trimLogged {
            trimLogged = true
            logAlways("output backlog passed \(Self.maxBacklogBytes) bytes, dropping the oldest")
        }
        deliverOutput()
    }

    /// Sends the next chunk unless one is on its way. The page answering is the flow control:
    /// each answer sends the next chunk, always of the current epoch, so nothing queued before
    /// an epoch change goes out after it.
    private func deliverOutput() {
        guard writeInFlight == nil, !backlog.isEmpty else { return }
        let state = readShared()
        guard state.isLoaded, state.hostSizeReady else {
            if !outputHeldLogged {
                outputHeldLogged = true
                tabInitLog("Hterm[\(instanceId)] holding output bytes=\(backlog.byteCount) loaded=\(state.isLoaded) hostReady=\(state.hostSizeReady)")
            }
            return
        }
        if outputHeldLogged {
            outputHeldLogged = false
            tabInitLog("Hterm[\(instanceId)] releasing held output bytes=\(backlog.byteCount)")
        }

        let chunk = backlog.take(upTo: Self.maxChunkBytes)
        let pending = backlog.byteCount
        updateShared { $0.pendingBytes = pending }
        if Self.debugEnabled && !firstChunkLogged {
            firstChunkLogged = true
            NSLog("%@", "Hterm[\(instanceId)] first chunk sent, \(chunk.count) bytes")
        }

        lastWriteId += 1
        let writeId = lastWriteId
        writeInFlight = writeId
        let script = "exports.writeB64ForEpoch(\(epoch), \"\(chunk.base64EncodedString())\")"
        webView.evaluateJavaScript(script) { [weak self] _, error in
            guard let self else { return }
            if let error {
                self.logAlways("write failed: \(error.localizedDescription)")
            }
            // A different id means the page was restarted while this was out.
            guard self.writeInFlight == writeId else { return }
            self.writeInFlight = nil
            self.deliverOutput()
        }
    }

    // MARK: Page load and recovery

    private func loadPage() {
        guard let url = URL(string: "\(TerminalAssetSchemeHandler.scheme)://terminal/term.html") else {
            logAlways("no URL for the terminal page")
            return
        }
        tabInitLog("Hterm[\(instanceId)] loading page")
        if Self.debugEnabled {
            NSLog("%@", "Hterm[\(instanceId)] loading \(url.absoluteString)")
        }
        webView.load(URLRequest(url: url))
    }

    @MainActor
    private func pageDidLoad() {
        tabInitLog("Hterm[\(instanceId)] page reports load")
        if Self.debugEnabled {
            NSLog("%@", "Hterm[\(instanceId)] load message")
        }
        let wasLoaded = isLoaded
        updateShared { $0.isLoaded = true }
        writeInFlight = nil

        // A page starts at epoch 1 and ours only goes up, and the page drops any write stamped
        // with another epoch, which would lose a whole tab's output without a word. Two things
        // separate them: a snapshot that arrived before the page was ready, and a page that
        // comes back after web content died. Both end here. This goes first because the
        // callbacks just below can send a snapshot, and resetting after that would wipe it.
        callPage("exports.resetForEpoch(\(epoch))")

        onLoaded?()
        if !wasLoaded {
            pageLoadStateDidChange(true)
        }

        // Style has to reach the page before output: programs ask for the terminal's colours,
        // and until now the page's colours are transparent placeholders.
        sendStyle()
        sendPageFocus()
        deliverOutput()
        queryGrid(reason: "load")
        applyForcedGrid()
    }

    @MainActor
    private func pageLoadStateDidChange(_ loaded: Bool) {
        displayTarget?.pageLoadStateChanged(loaded)
        callbackHost?.deliverLoadState(loaded)
    }

    /// The page has gone. Queued output stays for the next page, and so do the style and focus.
    @MainActor
    fileprivate func recoverPage(reason: String) {
        logAlways("page lost (\(reason)), reloading")
        tabInitLog("Hterm[\(instanceId)] page lost: \(reason)")
        let wasLoaded = isLoaded
        updateShared { $0.isLoaded = false }
        pageGrid = nil
        // WebKit may never answer a write sent to a dead process.
        writeInFlight = nil
        if wasLoaded {
            pageLoadStateDidChange(false)
        }
        guard !reloadQueued else { return }
        reloadQueued = true
        DispatchQueue.main.asyncAfter(deadline: .now() + Self.reloadDelay) { [weak self] in
            guard let self else { return }
            self.reloadQueued = false
            tabInitLog("Hterm[\(self.instanceId)] reloading page")
            self.loadPage()
        }
    }

    // MARK: Messages from the page

    @MainActor
    fileprivate func handlePageMessage(name: String, body: Any) {
        switch name {
        case "load":
            pageDidLoad()
        case "syncFocus":
            displayTarget?.refreshPageFocus()
        case "focus":
            displayTarget?.requestKeyboardFocus()
        case "sendInput":
            if let text = body as? String {
                callbackHost?.deliverInput(text)
            }
        case "resize":
            pageReportedGrid(body)
        case "propUpdate":
            if let pair = body as? [Any], pair.count == 2,
               pair[0] as? String == "applicationCursor", let enabled = pair[1] as? Bool {
                displayTarget?.setApplicationCursor(enabled)
            }
        case "newScrollHeight":
            if let height = (body as? NSNumber)?.doubleValue {
                displayTarget?.applyPageScrollHeight(CGFloat(height))
            }
        case "newScrollTop":
            if let top = (body as? NSNumber)?.doubleValue {
                displayTarget?.applyPageScrollTop(CGFloat(top))
            }
        case "openLink":
            if let text = body as? String, let url = URL(string: text) {
                UIApplication.shared.open(url)
            }
        case "selectionChanged":
            displayTarget?.pageSelectionChanged(Self.selectionRect(from: body))
        case "log":
            if let text = body as? String {
                logAlways(text)
            }
        default:
            break
        }
    }

    private static func selectionRect(from body: Any) -> CGRect? {
        guard let object = body as? [String: Any],
              let x = object["x"] as? NSNumber,
              let y = object["y"] as? NSNumber,
              let width = object["width"] as? NSNumber,
              let height = object["height"] as? NSNumber else {
            return nil
        }
        return CGRect(x: x.doubleValue, y: y.doubleValue, width: width.doubleValue, height: height.doubleValue)
    }

    // MARK: Grid

    /// A container's usable size. Output waits for the first of these, because until then the
    /// web view is still at its enormous starting size.
    fileprivate func noteHostSize(_ size: CGSize) {
        let usable = CGSize(width: size.width > 0 ? size.width : 1,
                            height: size.height > 0 ? size.height : 1)
        let changed = usable != lastHostSize
        lastHostSize = usable
        if !readShared().hostSizeReady {
            updateShared { $0.hostSizeReady = true }
        }
        if changed {
            tabInitLog("Hterm[\(instanceId)] host size \(usable.width)x\(usable.height)")
            if Self.debugEnabled {
                NSLog("%@", "Hterm[\(instanceId)] host size \(usable.width)x\(usable.height)")
            }
            if isLoaded {
                queryGrid(reason: "host size")
            }
        }
        deliverOutput()
    }

    private func queryGrid(reason: String) {
        lastGridQuery += 1
        let query = lastGridQuery
        sshResizeLog("[ssh-resize] hterm[\(instanceId)] query (\(reason))")
        webView.evaluateJavaScript("exports.reflowAndGetSize ? exports.reflowAndGetSize() : exports.getSize()") { [weak self] result, error in
            guard let self, query == self.lastGridQuery else { return }
            if let error {
                self.logAlways("size query failed: \(error.localizedDescription)")
                return
            }
            guard result is [Any], let grid = TerminalGrid(pageValue: result) else { return }
            sshResizeLog("[ssh-resize] hterm[\(self.instanceId)] query result \(grid.columns)x\(grid.rows)")
            self.callbackHost?.deliverResize(columns: grid.columns, rows: grid.rows)
        }
    }

    @MainActor
    private func pageReportedGrid(_ body: Any) {
        guard let grid = TerminalGrid(pageValue: body) else {
            queryGrid(reason: "rejected resize")
            return
        }
        pageGrid = grid
        forwardToRuntime(grid)
        callbackHost?.deliverResize(columns: grid.columns, rows: grid.rows)
    }

    private func forwardToRuntime(_ grid: TerminalGrid) {
        guard resizeSessionId != 0 else {
            sshResizeLog("[ssh-resize] hterm[\(instanceId)] deferring \(grid.columns)x\(grid.rows), no session")
            gridAwaitingSession = grid
            return
        }
        sshResizeLog("[ssh-resize] hterm[\(instanceId)] forwarding \(grid.columns)x\(grid.rows) to session \(resizeSessionId)")
        PSCALRuntimeUpdateSessionWindowSize(resizeSessionId, Int32(grid.columns), Int32(grid.rows))
        runtimeGrid = grid
    }

    func setResizeSessionId(_ sessionId: UInt64) {
        sshResizeLog("[ssh-resize] hterm[\(instanceId)] session \(sessionId)")
        resizeSessionId = sessionId
        guard sessionId != 0 else { return }
        if let waiting = gridAwaitingSession {
            gridAwaitingSession = nil
            // If the runtime has been told something else since, that's the newer word.
            if runtimeGrid == nil || runtimeGrid == waiting {
                forwardToRuntime(waiting)
            }
        }
        // A session that binds late still has to learn the true size.
        if isLoaded {
            queryGrid(reason: "session bound")
        }
    }

    func noteRuntimeSize(columns: Int, rows: Int) {
        guard columns > 0, rows > 0 else { return }
        runtimeGrid = TerminalGrid(columns: columns, rows: rows)
    }

    func forceGridSize(columns: Int, rows: Int) {
        let request = TerminalGrid(columns: max(1, columns), rows: max(1, rows))
        guard request != forcedGrid else { return }
        forcedGrid = request
        sshResizeLog("[ssh-resize] hterm[\(instanceId)] forced grid \(request.columns)x\(request.rows)")
        if isLoaded {
            applyForcedGrid()
        }
    }

    private func applyForcedGrid() {
        guard let request = forcedGrid, request != pageGrid else { return }
        webView.evaluateJavaScript("exports.setGridSize(\(request.columns), \(request.rows))") { [weak self] _, error in
            guard let self else { return }
            if let error {
                self.logAlways("setGridSize failed: \(error.localizedDescription)")
                return
            }
            self.pageGrid = request
        }
    }

    // MARK: Style and focus

    fileprivate func applyStyle(font: UIFont, foreground: UIColor, background: UIColor) {
        let traits = webView.traitCollection
        let style: [String: Any] = [
            "foregroundColor": Self.cssColor(foreground, traits: traits, fallback: "#FFFFFF"),
            "backgroundColor": Self.cssColor(background, traits: traits, fallback: "#000000"),
            "fontFamily": font.familyName,
            "fontSize": Double(font.pointSize),
            "blinkCursor": false,
            "cursorShape": "BLOCK",
        ]
        guard let json = try? JSONSerialization.data(withJSONObject: style),
              let text = String(data: json, encoding: .utf8) else {
            return
        }
        styleScript = "exports.updateStyle(\(text))"
        if isLoaded {
            sendStyle()
        }
    }

    private static func cssColor(_ color: UIColor, traits: UITraitCollection, fallback: String) -> String {
        var red: CGFloat = 0
        var green: CGFloat = 0
        var blue: CGFloat = 0
        var alpha: CGFloat = 0
        guard color.resolvedColor(with: traits).getRed(&red, green: &green, blue: &blue, alpha: &alpha) else {
            return fallback
        }
        // Colours from an extended colour space can read outside 0...1.
        let byte = { (component: CGFloat) in UInt32(max(0, min(255, component * 255))) }
        return String(format: "#%02X%02X%02X", byte(red), byte(green), byte(blue))
    }

    private func sendStyle() {
        guard let styleScript else { return }
        webView.evaluateJavaScript(styleScript) { [weak self] _, error in
            guard let self else { return }
            if let error {
                self.logAlways("updateStyle failed: \(error.localizedDescription)")
            }
            // Early grid reports can be stale while the new font settles, so ask again. hterm
            // also measures its own grid, so a forced one has to be put back.
            DispatchQueue.main.asyncAfter(deadline: .now() + Self.styleSettleDelay) { [weak self] in
                guard let self, self.isLoaded else { return }
                self.queryGrid(reason: "style")
                self.applyForcedGrid()
            }
        }
    }

    fileprivate func setPageFocused(_ focused: Bool) {
        wantsPageFocus = focused
        if isLoaded {
            sendPageFocus()
        }
    }

    private func sendPageFocus() {
        callPage("exports.setFocused(\(wantsPageFocus))")
    }

    // MARK: Calls from the container

    fileprivate func bindDisplay(_ container: HtermTerminalContainerView) {
        displayTarget = container
    }

    fileprivate func unbindDisplay(_ container: HtermTerminalContainerView) {
        if displayTarget === container {
            displayTarget = nil
        }
    }

    fileprivate func adoptCallbacks(from container: HtermTerminalContainerView) {
        callbackHost = container
    }

    fileprivate func setEditActions(copy: @escaping () -> Void, paste: @escaping (String) -> Void) {
        terminalWebView.copyAction = copy
        terminalWebView.pasteAction = paste
    }

    /// The page should treat this as user activity and stop showing history.
    fileprivate func noteUserInput() {
        guard isLoaded else { return }
        callPage("exports.setUserGesture()")
        // Input can arrive far faster than it's worth scrolling for.
        guard !scrollToBottomQueued else { return }
        scrollToBottomQueued = true
        DispatchQueue.main.async { [weak self] in
            guard let self else { return }
            self.scrollToBottomQueued = false
            if self.isLoaded {
                self.callPage("exports.scrollToBottom()")
            }
        }
    }

    fileprivate func copySelection() {
        if isLoaded {
            callPage("exports.copy()")
        }
    }

    fileprivate func movePageScrollTop(_ top: CGFloat) {
        guard top.isFinite else { return }
        // Goes out whatever the load state, and a page that isn't there just fails.
        webView.evaluateJavaScript("exports.newScrollTop(\(top))", completionHandler: nil)
    }

    fileprivate func applyWebViewSettings() {
        webView.isOpaque = false
        webView.backgroundColor = .clear
        let inner = webView.scrollView
        inner.isOpaque = false
        inner.backgroundColor = .clear
        inner.isScrollEnabled = false
        inner.bounces = false
        inner.delaysContentTouches = false
        inner.canCancelContentTouches = false
        inner.panGestureRecognizer.isEnabled = false
    }

    // MARK: Memory pressure

    /// All three signals, because none covers the others: the dispatch source fires first on a
    /// device but follows the Mac's memory in the simulator, the notification is the documented
    /// warning, and only the diagnostics request can be raised on demand.
    private func startMemorySignals() {
        let source = DispatchSource.makeMemoryPressureSource(eventMask: [.warning, .critical], queue: .main)
        source.setEventHandler { [weak self, weak source] in
            guard let event = source?.data else { return }
            self?.releaseScrollback(critical: event.contains(.critical))
        }
        source.resume()
        memorySource = source

        let center = NotificationCenter.default
        center.addObserver(self, selector: #selector(memorySignalReceived),
                           name: UIApplication.didReceiveMemoryWarningNotification, object: nil)
        center.addObserver(self, selector: #selector(memorySignalReceived),
                           name: Self.releaseScrollbackRequest, object: nil)
    }

    @objc private func memorySignalReceived() {
        onMain { [weak self] in
            self?.releaseScrollback(critical: false)
        }
    }

    private func releaseScrollback(critical: Bool) {
        runtimeDebugLog("[Hterm \(instanceId)] memory pressure signalled (critical=\(critical))")
        guard isLoaded else {
            runtimeDebugLog("[Hterm \(instanceId)] page not loaded, nothing to release")
            return
        }
        let severity = critical ? "critical" : "warning"
        webView.evaluateJavaScript("exports.releaseMemory(\(critical ? 200 : 1000))") { [weak self] result, error in
            guard let self else { return }
            if let error {
                self.logAlways("releaseMemory failed: \(error.localizedDescription)")
                return
            }
            let counts = result as? [NSNumber] ?? []
            let before = counts.first?.intValue ?? -1
            let after = counts.count > 1 ? counts[1].intValue : -1
            let line = "[Hterm \(self.instanceId)] \(severity) memory pressure, scrollback \(before) -> \(after) rows"
            runtimeDebugLog(line)
            NSLog("%@", line)
        }
    }

    // MARK: Helpers

    /// Calls a page function that has nothing to return.
    private func callPage(_ script: String) {
        webView.evaluateJavaScript(script) { [weak self] _, error in
            if let error {
                self?.logAlways("\(script) failed: \(error.localizedDescription)")
            }
        }
    }

    private func logAlways(_ message: String) {
        NSLog("%@", "[Hterm \(instanceId)] \(message)")
    }
}

extension HtermTerminalController: WKNavigationDelegate {
    func webView(_ webView: WKWebView, didFail navigation: WKNavigation!, withError error: Error) {
        navigationFailed(error)
    }

    func webView(_ webView: WKWebView, didFailProvisionalNavigation navigation: WKNavigation!, withError error: Error) {
        navigationFailed(error)
    }

    func webViewWebContentProcessDidTerminate(_ webView: WKWebView) {
        recoverPage(reason: "web content process terminated")
    }

    @MainActor
    private func navigationFailed(_ error: Error) {
        let failure = error as NSError
        if failure.domain == NSURLErrorDomain && failure.code == NSURLErrorCancelled {
            return
        }
        // The termination callback already deals with this one.
        if failure.domain == WKErrorDomain && failure.code == WKError.Code.webContentProcessTerminated.rawValue {
            return
        }
        recoverPage(reason: failure.localizedDescription)
    }
}

/// WebKit keeps its message handlers alive, so the controller can't be one and ever be freed.
private final class PageMessageRelay: NSObject {
    weak var controller: HtermTerminalController?
}

extension PageMessageRelay: WKScriptMessageHandler {
    func userContentController(_ userContentController: WKUserContentController, didReceive message: WKScriptMessage) {
        controller?.handlePageMessage(name: message.name, body: message.body)
    }
}

/// Serves the page's files out of the app bundle.
private final class TerminalAssetSchemeHandler: NSObject {
    static let shared = TerminalAssetSchemeHandler()
    static let scheme = "pscal-terminal"

    private static let mimeTypes = [
        "term.html": "text/html",
        "term.css": "text/css",
        "term.js": "application/javascript",
        "hterm_all.js": "application/javascript",
    ]

    private let cacheLock = NSLock()
    private var cache: [String: Data] = [:]

    private func contents(of name: String) -> Data? {
        cacheLock.lock()
        defer { cacheLock.unlock() }
        if let cached = cache[name] {
            return cached
        }
        guard let url = Bundle.main.url(forResource: name, withExtension: nil, subdirectory: "TerminalWeb"),
              let data = try? Data(contentsOf: url) else {
            return nil
        }
        cache[name] = data
        return data
    }

    fileprivate func serve(_ task: WKURLSchemeTask) {
        guard let url = task.request.url else {
            task.didFailWithError(NSError(domain: NSURLErrorDomain, code: NSURLErrorBadURL))
            return
        }
        let last = url.lastPathComponent
        let name = (last.isEmpty || last == "/") ? "term.html" : last
        guard let mimeType = Self.mimeTypes[name], let data = contents(of: name) else {
            task.didFailWithError(NSError(domain: NSURLErrorDomain,
                                          code: NSURLErrorFileDoesNotExist,
                                          userInfo: [NSLocalizedDescriptionKey: "No terminal resource named \(name)"]))
            return
        }
        task.didReceive(URLResponse(url: url, mimeType: mimeType, expectedContentLength: data.count, textEncodingName: "utf-8"))
        task.didReceive(data)
        task.didFinish()
    }
}

extension TerminalAssetSchemeHandler: WKURLSchemeHandler {
    func webView(_ webView: WKWebView, start urlSchemeTask: WKURLSchemeTask) {
        serve(urlSchemeTask)
    }

    func webView(_ webView: WKWebView, stop urlSchemeTask: WKURLSchemeTask) {}
}

/// A web view that routes the edit menu back to native, because the page must never paste for
/// itself: native decides on bracketed-paste wrapping.
private final class TerminalWebView: WKWebView {
    var copyAction: (() -> Void)?
    var pasteAction: ((String) -> Void)?

    override var canBecomeFirstResponder: Bool { true }

    override func canPerformAction(_ action: Selector, withSender sender: Any?) -> Bool {
        switch action {
        case #selector(copy(_:)), #selector(cut(_:)), #selector(selectAll(_:)):
            return true
        case #selector(paste(_:)):
            return UIPasteboard.general.hasStrings
        default:
            return super.canPerformAction(action, withSender: sender)
        }
    }

    override func copy(_ sender: Any?) {
        if let copyAction {
            copyAction()
        } else {
            super.copy(sender)
        }
    }

    override func cut(_ sender: Any?) {
        if let copyAction {
            copyAction()
        } else {
            super.cut(sender)
        }
    }

    override func paste(_ sender: Any?) {
        guard let text = UIPasteboard.general.string, !text.isEmpty else { return }
        if let pasteAction {
            pasteAction(text)
        } else {
            super.paste(sender)
        }
    }
}

/// A scroll view whose content view doesn't scroll. The web view must fill the visible area and
/// stay put while the indicator and the offset behave as if something much taller were being
/// scrolled, so the content view's frame follows the offset.
///
/// This hooks the setters rather than `scrollViewDidScroll` so that it needs no say over
/// `delegate`, which stays an ordinary delegate: UIKit calls the delegate from inside the
/// `contentOffset` setter, and by then the content view has already moved.
private final class PinnedContentScrollView: UIScrollView {
    /// Assigning doesn't add it as a subview; that's up to the caller.
    var contentView: UIView? {
        didSet { contentBase = contentView?.frame.origin ?? .zero }
    }

    private var contentBase = CGPoint.zero

    override var contentOffset: CGPoint {
        get { super.contentOffset }
        set {
            pinContentView(atOffset: newValue)
            super.contentOffset = newValue
        }
    }

    override var bounds: CGRect {
        get { super.bounds }
        set {
            pinContentView(atOffset: newValue.origin)
            super.bounds = newValue
        }
    }

    override func layoutSubviews() {
        super.layoutSubviews()
        pinContentView()
    }

    func pinContentView() {
        pinContentView(atOffset: contentOffset)
    }

    private func pinContentView(atOffset offset: CGPoint) {
        guard let contentView else { return }
        let origin = CGPoint(x: contentBase.x + offset.x, y: contentBase.y + offset.y)
        if contentView.frame.origin != origin {
            contentView.frame.origin = origin
        }
    }
}

/// Shows a terminal: the scroll view holding the web view, and the hidden text view that
/// takes the keyboard. Being on screen and having the keyboard are separate here, because
/// Workspace shows several terminals at once and at most one of them has the keyboard.
@MainActor
final class HtermTerminalContainerView: UIView, UIScrollViewDelegate, UIGestureRecognizerDelegate {
    private static let focusRetryDelay: TimeInterval = 1.05

    private let controller: HtermTerminalController
    private let scrollHost = PinnedContentScrollView()
    private let keyInput = TerminalKeyInputView()

    private var inputCallback: ((String) -> Void)?
    private var pasteCallback: ((String) -> Void)?
    private var resizeCallback: ((Int, Int) -> Void)?
    private var loadStateCallback: ((Bool) -> Void)?
    private var attachCallback: ((HtermTerminalController) -> Void)?
    private var detachCallback: ((HtermTerminalController) -> Void)?

    private var onScreen = true
    private var isActive = true
    private var displayed = false
    private var inWindow = false
    private var knownPageLoaded = false
    private var lastFocusToken = 0

    private var focusWaiting = false
    private var focusRetryQueued = false

    private var movingScrollForPage = false
    private var lastSelectionRect: CGRect?
    private var showedSelectionMenu = false

    init(controller: HtermTerminalController) {
        self.controller = controller
        super.init(frame: .zero)
        isOpaque = false
        setUpScrollHost()
        setUpKeyInput()

        let tap = UITapGestureRecognizer(target: self, action: #selector(handleTap))
        tap.cancelsTouchesInView = false
        tap.delegate = self
        addGestureRecognizer(tap)

        let center = NotificationCenter.default
        center.addObserver(self, selector: #selector(windowKeyStatusChanged(_:)),
                           name: UIWindow.didBecomeKeyNotification, object: nil)
        center.addObserver(self, selector: #selector(windowKeyStatusChanged(_:)),
                           name: UIWindow.didResignKeyNotification, object: nil)
    }

    @available(*, unavailable)
    required init?(coder: NSCoder) {
        fatalError("init(coder:) is not supported")
    }

    deinit {
        if inWindow {
            detachCallback?(controller)
        }
    }

    private func setUpScrollHost() {
        scrollHost.frame = bounds
        scrollHost.autoresizingMask = [.flexibleWidth, .flexibleHeight]
        scrollHost.bounces = false
        scrollHost.alwaysBounceVertical = false
        scrollHost.showsVerticalScrollIndicator = true
        scrollHost.showsHorizontalScrollIndicator = false
        scrollHost.delaysContentTouches = false
        scrollHost.canCancelContentTouches = false
        scrollHost.isOpaque = false
        scrollHost.backgroundColor = .clear
        scrollHost.clipsToBounds = true
        scrollHost.contentInsetAdjustmentBehavior = .never
        scrollHost.delegate = self
        addSubview(scrollHost)
    }

    private func setUpKeyInput() {
        keyInput.translatesAutoresizingMaskIntoConstraints = false
        keyInput.backgroundColor = .clear
        keyInput.textColor = .clear
        keyInput.tintColor = .clear
        keyInput.isScrollEnabled = false
        keyInput.text = ""
        keyInput.autocorrectionType = .no
        keyInput.autocapitalizationType = .none
        keyInput.spellCheckingType = .no
        keyInput.smartQuotesType = .no
        keyInput.smartInsertDeleteType = .no
        keyInput.smartDashesType = .no
        keyInput.keyboardAppearance = .dark
        keyInput.inputAssistantItem.leadingBarButtonGroups = []
        keyInput.inputAssistantItem.trailingBarButtonGroups = []
        // Until the first update says whether this terminal is the active one.
        keyInput.inputEnabled = false

        keyInput.onInput = { [weak self] text in
            self?.deliverInput(text)
        }
        keyInput.onPaste = { [weak self] text in
            self?.deliverPaste(text)
        }
        keyInput.onCopy = { [weak self] in
            self?.controller.copySelection()
        }
        keyInput.onCloseTab = {
            TerminalTabManager.shared.closeSelectedTab()
        }
        keyInput.onNewTab = {
            _ = TerminalTabManager.shared.openShellTab()
        }
        keyInput.onFocusChange = { [weak self] focused in
            self?.controller.setPageFocused(focused)
        }

        addSubview(keyInput)
        NSLayoutConstraint.activate([
            keyInput.leadingAnchor.constraint(equalTo: leadingAnchor),
            keyInput.bottomAnchor.constraint(equalTo: bottomAnchor),
            keyInput.widthAnchor.constraint(equalToConstant: 1),
            keyInput.heightAnchor.constraint(equalToConstant: 1),
        ])
    }

    // MARK: Updates from SwiftUI

    func update(from view: HtermTerminalView) {
        backgroundColor = view.backgroundColor

        inputCallback = view.onInput
        pasteCallback = view.onPaste
        resizeCallback = view.onResize
        loadStateCallback = view.onLoadStateChange
        attachCallback = view.onReady
        detachCallback = view.onDetach
        // Handed over as they are: the key view has its own fallback when one of these is nil.
        keyInput.onInterrupt = view.onInterrupt
        keyInput.onSuspend = view.onSuspend

        controller.adoptCallbacks(from: self)
        controller.setEditActions(copy: { [weak self] in
            self?.controller.copySelection()
        }, paste: { [weak self] text in
            self?.deliverPaste(text)
        })

        applyPresentation(isVisible: view.isVisible, isActive: view.isActive)
        controller.applyStyle(font: view.font, foreground: view.foregroundColor, background: view.backgroundColor)
        pageLoadStateChanged(controller.isLoaded)

        if view.focusToken != lastFocusToken {
            lastFocusToken = view.focusToken
            requestKeyboardFocus()
        }
    }

    /// SwiftUI repeats these on every update, so only a change may do anything.
    private func applyPresentation(isVisible: Bool, isActive: Bool) {
        let activeChanged = isActive != self.isActive
        let visibleChanged = isVisible != onScreen
        self.isActive = isActive
        onScreen = isVisible
        keyInput.inputEnabled = isActive
        guard activeChanged || visibleChanged else { return }

        tabInitLog("HtermView[\(controller.instanceId)] visible=\(isVisible) active=\(isActive)")
        if activeChanged && !isActive {
            focusWaiting = false
            controller.setPageFocused(false)
        }
        if visibleChanged {
            refreshDisplayedState()
        }
    }

    // MARK: Calls from the controller

    func deliverInput(_ text: String) {
        inputCallback?(text)
        controller.noteUserInput()
    }

    private func deliverPaste(_ text: String) {
        pasteCallback?(text)
        controller.noteUserInput()
    }

    func deliverResize(columns: Int, rows: Int) {
        resizeCallback?(columns, rows)
    }

    func deliverLoadState(_ loaded: Bool) {
        loadStateCallback?(loaded)
    }

    func setApplicationCursor(_ enabled: Bool) {
        keyInput.applicationCursorEnabled = enabled
    }

    func pageLoadStateChanged(_ loaded: Bool) {
        guard loaded != knownPageLoaded else { return }
        knownPageLoaded = loaded
        keyInput.inputEnabled = isActive
        guard loaded else { return }
        if displayed && !isHostingWebView {
            installWebView()
        }
        reportHostSizeOnceSized()
        honourFocusRequest()
    }

    /// For the reports that don't come from a layout pass. Before the first layout there is no
    /// size to report, and calling that a size would release held output too early. The layout
    /// pass will report it.
    private func reportHostSizeOnceSized() {
        if !scrollHost.bounds.isEmpty {
            controller.noteHostSize(scrollHost.bounds.size)
        }
    }

    // MARK: Showing the web view

    private var isHostingWebView: Bool {
        controller.webView.superview === scrollHost
    }

    /// Whether the web view belongs here. It stays out of a non-key window, but it doesn't wait
    /// for the page: WebKit may hold off loading a page whose view isn't in a window.
    private func refreshDisplayedState() {
        let shouldDisplay = onScreen && window?.isKeyWindow == true
        guard shouldDisplay != displayed else {
            // Another container that took this controller over has gone away again.
            if displayed && controller.webView.superview == nil {
                controller.bindDisplay(self)
                installWebView()
            }
            return
        }
        displayed = shouldDisplay
        if shouldDisplay {
            controller.bindDisplay(self)
            installWebView()
            honourFocusRequest()
        } else {
            if keyInput.isFirstResponder {
                _ = keyInput.resignFirstResponder()
            }
            removeWebView()
            // Another container may have taken this controller over by now.
            controller.unbindDisplay(self)
        }
    }

    private func installWebView() {
        let web = controller.webView
        tabInitLog("HtermView[\(controller.instanceId)] install")
        // The previous host must stop positioning it.
        (web.superview as? PinnedContentScrollView)?.contentView = nil
        web.removeFromSuperview()
        web.translatesAutoresizingMaskIntoConstraints = true
        web.frame = scrollHost.bounds
        web.autoresizingMask = [.flexibleWidth, .flexibleHeight]
        controller.applyWebViewSettings()
        scrollHost.contentView = web
        // Underneath, so that it can't cover the scroll indicator.
        scrollHost.insertSubview(web, at: 0)
        scrollHost.pinContentView()
        reportHostSizeOnceSized()
    }

    private func removeWebView() {
        guard isHostingWebView else { return }
        tabInitLog("HtermView[\(controller.instanceId)] uninstall")
        controller.webView.removeFromSuperview()
        scrollHost.contentView = nil
    }

    override func layoutSubviews() {
        super.layoutSubviews()
        if isHostingWebView {
            controller.webView.frame.size = scrollHost.bounds.size
            scrollHost.pinContentView()
        }
        controller.noteHostSize(scrollHost.bounds.size)
        clampScrollOffset()
        refreshDisplayedState()
        trace("layout bounds=\(scrollHost.bounds) content=\(scrollHost.contentSize)")
    }

    override func didMoveToWindow() {
        super.didMoveToWindow()
        if window != nil {
            if !inWindow {
                inWindow = true
                tabInitLog("HtermView[\(controller.instanceId)] attach")
                attachCallback?(controller)
            }
        } else if inWindow {
            inWindow = false
            tabInitLog("HtermView[\(controller.instanceId)] detach")
            detachCallback?(controller)
        }
        refreshPageFocus()
        refreshDisplayedState()
        honourFocusRequest()
    }

    // MARK: Keyboard and focus

    /// Asks for the keyboard. Without the right to take it now, the request waits until
    /// something changes.
    func requestKeyboardFocus() {
        focusWaiting = true
        honourFocusRequest()
    }

    private func honourFocusRequest() {
        guard focusWaiting, mayTakeKeyboard else { return }
        if keyInput.isFirstResponder || keyInput.becomeFirstResponder() {
            focusWaiting = false
            return
        }
        guard !focusRetryQueued else { return }
        focusRetryQueued = true
        DispatchQueue.main.asyncAfter(deadline: .now() + Self.focusRetryDelay) { [weak self] in
            guard let self else { return }
            self.focusRetryQueued = false
            self.honourFocusRequest()
        }
    }

    private var mayTakeKeyboard: Bool {
        guard isActive, onScreen, let window,
              UIApplication.shared.applicationState == .active,
              pscalIOSSDLModeActive() == 0,
              !Self.sdlWindowIsShowing else {
            return false
        }
        // On iPad a terminal can join its window before that window is key, and nobody should
        // have to tap to get a keyboard then.
        return window.isKeyWindow || !Self.hasKeyWindow(in: window.windowScene, other: window)
    }

    private static var sdlWindowIsShowing: Bool {
        UIApplication.shared.connectedScenes
            .compactMap { $0 as? UIWindowScene }
            .flatMap { $0.windows }
            .contains { window in
                guard !window.isHidden else { return false }
                let name = NSStringFromClass(type(of: window)).lowercased()
                return name.contains("sdl") && name.contains("window")
            }
    }

    private static func hasKeyWindow(in scene: UIWindowScene?, other window: UIWindow?) -> Bool {
        scene?.windows.contains { $0 !== window && $0.isKeyWindow } ?? false
    }

    /// Tells the page whether it counts as focused, which is more than having the keyboard: a
    /// terminal keeps a solid cursor even while its web view is not first responder.
    func refreshPageFocus() {
        let focused = window?.isKeyWindow == true && isActive && onScreen
        controller.setPageFocused(focused)
        if !focused && keyInput.isFirstResponder {
            _ = keyInput.resignFirstResponder()
        }
    }

    @objc private func windowKeyStatusChanged(_ notification: Notification) {
        guard let window, notification.object as? UIWindow === window else { return }
        // Key can drop away briefly with nothing else taking it. Tearing the terminal down for
        // that would make it flicker.
        if notification.name == UIWindow.didResignKeyNotification,
           UIApplication.shared.applicationState == .active,
           !Self.hasKeyWindow(in: window.windowScene, other: nil) {
            return
        }
        refreshPageFocus()
        refreshDisplayedState()
    }

    @objc private func handleTap() {
        requestKeyboardFocus()
    }

    // WebKit has tap recognisers of its own, and this one must not lose to them.
    func gestureRecognizer(_ gestureRecognizer: UIGestureRecognizer,
                           shouldRecognizeSimultaneouslyWith otherGestureRecognizer: UIGestureRecognizer) -> Bool {
        true
    }

    // MARK: Selection menu

    func pageSelectionChanged(_ rect: CGRect?) {
        DispatchQueue.main.async { [weak self] in
            self?.updateSelectionMenu(for: rect)
        }
    }

    private func updateSelectionMenu(for rect: CGRect?) {
        let menu = UIMenuController.shared
        guard let rect else {
            guard showedSelectionMenu else { return }
            showedSelectionMenu = false
            lastSelectionRect = nil
            menu.hideMenu()
            if isActive {
                _ = keyInput.becomeFirstResponder()
            }
            return
        }
        if rect == lastSelectionRect && menu.isMenuVisible {
            return
        }
        let web = controller.webView
        if keyInput.isFirstResponder {
            _ = keyInput.resignFirstResponder()
        }
        _ = web.becomeFirstResponder()
        let grown = rect.insetBy(dx: -4, dy: -4)
        let clipped = grown.intersection(web.bounds)
        menu.showMenu(from: web, rect: clipped.isEmpty ? grown : clipped)
        lastSelectionRect = rect
        showedSelectionMenu = true
    }

    // MARK: Scroll sync

    func applyPageScrollHeight(_ height: CGFloat) {
        guard isHostingWebView else { return }
        let usable = (height.isFinite && height > 0) ? height : 0
        let width = scrollHost.bounds.width > 0 ? scrollHost.bounds.width : max(scrollHost.contentSize.width, 1)
        movingScrollForPage = true
        scrollHost.contentSize = CGSize(width: width, height: usable)
        movingScrollForPage = false
        clampScrollOffset()
        trace("page height \(usable)")
    }

    func applyPageScrollTop(_ top: CGFloat) {
        guard isHostingWebView, top.isFinite else { return }
        let limit = max(0, scrollHost.contentSize.height - scrollHost.bounds.height)
        let target = min(max(0, top), limit)
        guard abs(scrollHost.contentOffset.y - target) >= 0.5 else { return }
        moveScrollHostQuietly(to: CGPoint(x: scrollHost.contentOffset.x, y: target))
        trace("page top \(target)")
    }

    private func clampScrollOffset() {
        let offset = scrollHost.contentOffset
        let limitX = max(0, scrollHost.contentSize.width - scrollHost.bounds.width)
        let limitY = max(0, scrollHost.contentSize.height - scrollHost.bounds.height)
        let clamped = CGPoint(x: min(max(0, offset.x), limitX), y: min(max(0, offset.y), limitY))
        if abs(offset.x - clamped.x) < 0.5 && abs(offset.y - clamped.y) < 0.5 {
            scrollHost.pinContentView()
        } else {
            moveScrollHostQuietly(to: clamped)
        }
    }

    /// Moves the scroll view without telling the page, which is where the move came from or
    /// which has no need to hear about a clamp.
    private func moveScrollHostQuietly(to offset: CGPoint) {
        movingScrollForPage = true
        scrollHost.setContentOffset(offset, animated: false)
        movingScrollForPage = false
        scrollHost.pinContentView()
    }

    func scrollViewDidScroll(_ scrollView: UIScrollView) {
        guard !movingScrollForPage else { return }
        trace("user scroll \(scrollView.contentOffset.y)")
        controller.movePageScrollTop(scrollView.contentOffset.y)
    }

    private func trace(_ message: @autoclosure () -> String) {
        guard HtermTerminalController.debugEnabled else { return }
        fputs("HtermView[\(controller.instanceId)] \(message())\n", stderr)
    }
}

/// The terminal as SwiftUI sees it. The controller given when the view is first created is the
/// one it keeps, whatever later updates pass.
struct HtermTerminalView: UIViewRepresentable {
    let controller: HtermTerminalController
    let font: UIFont
    let foregroundColor: UIColor
    let backgroundColor: UIColor
    var focusToken: Int = 0
    var isVisible: Bool = true
    var isActive: Bool = true
    var onInput: ((String) -> Void)? = nil
    var onPaste: ((String) -> Void)? = nil
    var onInterrupt: (() -> Void)? = nil
    var onSuspend: (() -> Void)? = nil
    var onResize: ((Int, Int) -> Void)? = nil
    var onReady: ((HtermTerminalController) -> Void)? = nil
    var onDetach: ((HtermTerminalController) -> Void)? = nil
    var onLoadStateChange: ((Bool) -> Void)? = nil

    func makeUIView(context: Context) -> HtermTerminalContainerView {
        let container = HtermTerminalContainerView(controller: controller)
        container.update(from: self)
        return container
    }

    func updateUIView(_ uiView: HtermTerminalContainerView, context: Context) {
        uiView.update(from: self)
    }
}
