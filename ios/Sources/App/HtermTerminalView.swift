import SwiftUI
import WebKit
import UIKit
import Foundation

private final class TerminalResourceSchemeHandler: NSObject, WKURLSchemeHandler {
    private struct Resource {
        let data: Data
        let mimeType: String
    }

    private let cacheQueue = DispatchQueue(label: "com.pscal.hterm.scheme-cache")
    private var cache: [String: Resource] = [:]

    func webView(_ webView: WKWebView, start urlSchemeTask: WKURLSchemeTask) {
        guard let url = urlSchemeTask.request.url else {
            urlSchemeTask.didFailWithError(NSError(domain: NSURLErrorDomain,
                                                   code: NSURLErrorBadURL,
                                                   userInfo: nil))
            return
        }
        let name = url.lastPathComponent.isEmpty ? "term.html" : url.lastPathComponent
        guard let resource = loadResource(named: name) else {
            let error = NSError(domain: NSURLErrorDomain,
                                code: NSURLErrorFileDoesNotExist,
                                userInfo: [NSLocalizedDescriptionKey: "Missing terminal resource \(name)"])
            urlSchemeTask.didFailWithError(error)
            return
        }
        let response = URLResponse(url: url,
                                   mimeType: resource.mimeType,
                                   expectedContentLength: resource.data.count,
                                   textEncodingName: "utf-8")
        urlSchemeTask.didReceive(response)
        urlSchemeTask.didReceive(resource.data)
        urlSchemeTask.didFinish()
    }

    func webView(_ webView: WKWebView, stop urlSchemeTask: WKURLSchemeTask) {
        // Nothing to clean up.
    }

    private func loadResource(named name: String) -> Resource? {
        let normalized = name.isEmpty ? "term.html" : name
        if let cached = cacheQueue.sync(execute: { cache[normalized] }) {
            return cached
        }

        let mimeType: String
        switch normalized {
        case "term.html":
            mimeType = "text/html"
        case "term.css":
            mimeType = "text/css"
        case "term.js", "hterm_all.js":
            mimeType = "application/javascript"
        default:
            return nil
        }

        guard let url = Bundle.main.url(forResource: normalized, withExtension: nil, subdirectory: "TerminalWeb"),
              let data = try? Data(contentsOf: url) else {
            return nil
        }
        let resource = Resource(data: data, mimeType: mimeType)
        cacheQueue.async { [resource] in
            self.cache[normalized] = resource
        }
        return resource
    }
}

private final class HtermWebView: WKWebView {
    var onPaste: ((String) -> Void)?
    var onCopy: (() -> Void)?

    override var canBecomeFirstResponder: Bool {
        true
    }

    override func becomeFirstResponder() -> Bool {
        super.becomeFirstResponder()
    }

    override func canPerformAction(_ action: Selector, withSender sender: Any?) -> Bool {
        if action == #selector(copy(_:)) || action == #selector(cut(_:)) {
            return true
        }
        if action == #selector(paste(_:)) {
            return UIPasteboard.general.hasStrings
        }
        if action == #selector(selectAll(_:)) {
            return true
        }
        return super.canPerformAction(action, withSender: sender)
    }

    override func copy(_ sender: Any?) {
        if let onCopy {
            onCopy()
            return
        }
        super.copy(sender)
    }

    override func cut(_ sender: Any?) {
        if let onCopy {
            onCopy()
            return
        }
        super.cut(sender)
    }

    override func paste(_ sender: Any?) {
        guard let text = UIPasteboard.general.string, !text.isEmpty else { return }
        if let onPaste {
            onPaste(text)
            return
        }
        super.paste(sender)
    }
}

final class HtermTerminalController: NSObject, WKScriptMessageHandler, WKNavigationDelegate {
    private enum HandlerName: String, CaseIterable {
        case load
        case log
        case sendInput
        case resize
        case propUpdate
        case syncFocus
        case focus
        case newScrollHeight
        case newScrollTop
        case openLink
        case selectionChanged
    }

    private struct TerminalStylePayload {
        let json: String
    }

    private let outputLock = NSLock()
    private var pendingOutput = Data()
    private var outputInProgress = false
    private var reloadPending = false
    private var didLogFirstOutput = false
    private var didLogFirstFlush = false
    private let maxFlushBytes = 32 * 1024
    private var hostSizeReady = false
    private var hostSize = CGSize.zero

    private(set) var isLoaded = false
    private var pendingStyle: TerminalStylePayload?
    private var pendingFocus: Bool?
    private var lastStyle: TerminalStylePayload?
    private var lastFocus: Bool?
    private var applicationCursor = false
    private var scrollToBottomPending = false
    private var resizeRequestGeneration: UInt64 = 0
    private var pendingForcedGridSize: (columns: Int, rows: Int)?
    private var resizeSessionId: UInt64 = 0
    private var pendingRuntimeResize: (columns: Int, rows: Int, source: String)?

    private static let terminalScheme = "pscal-terminal"
    private static let schemeHandler = TerminalResourceSchemeHandler()
    private static let sharedProcessPool = WKProcessPool()
    let webView: WKWebView
    private let userContentController: WKUserContentController
    private var scriptMessageBridges: [WeakScriptMessageHandler] = []
    var onInput: ((String) -> Void)?
    var onResize: ((Int, Int) -> Void)?
    var onFocusRequested: (() -> Void)?
    var onSyncFocus: (() -> Void)?
    var onLoaded: (() -> Void)?
    var onLoadStateChange: ((Bool) -> Void)?
    var onScrollHeight: ((CGFloat) -> Void)?
    var onScrollTop: ((CGFloat) -> Void)?
    var onApplicationCursorChange: ((Bool) -> Void)?
    fileprivate weak var displayContainer: HtermTerminalContainerView?

    static let debugEnabled: Bool = {
        let env = ProcessInfo.processInfo.environment
        if let value = env["PSCALI_HERM_DEBUG"] {
            return value != "0"
        }
        if let value = env["PSCALI_PTY_OUTPUT_LOG"] {
            return value != "0"
        }
        return false
    }()
    private static var nextInstanceId: Int = 0
    let instanceId: Int
    private var didLogZeroHostSize = false
    private var lastFlushBlockReason: String?

    static func makeOnMainThread() -> HtermTerminalController {
        if Thread.isMainThread {
            return HtermTerminalController()
        }
        var controller: HtermTerminalController?
        DispatchQueue.main.sync {
            controller = HtermTerminalController()
        }
        return controller!
    }

    override init() {
        Self.nextInstanceId += 1
        self.instanceId = Self.nextInstanceId
        let controller = WKUserContentController()
        let config = WKWebViewConfiguration()
        // Keep one process pool for all terminal tabs to avoid high-memory
        // churn when users open/close tabs rapidly under load.
        config.processPool = Self.sharedProcessPool
        config.setURLSchemeHandler(Self.schemeHandler, forURLScheme: Self.terminalScheme)
        config.userContentController = controller
        let debugValue = Self.debugEnabled ? "true" : "false"
        let debugScript = WKUserScript(
            source: "window.PSCALI_HERM_DEBUG = \(debugValue);",
            injectionTime: .atDocumentStart,
            forMainFrameOnly: true
        )
        controller.addUserScript(debugScript)
        let instanceScript = WKUserScript(
            source: "window.PSCALI_HERM_INSTANCE_ID = \(instanceId);",
            injectionTime: .atDocumentStart,
            forMainFrameOnly: true
        )
        controller.addUserScript(instanceScript)
        let initialFrame = CGRect(x: 0, y: 0, width: 10000, height: 10000)
        self.webView = HtermWebView(frame: initialFrame, configuration: config)
        self.userContentController = controller
        super.init()
        logTabInit("controller init thread=\(Thread.isMainThread ? "main" : "bg")")
        let bridges = HandlerName.allCases.map { _ in WeakScriptMessageHandler(handler: self) }
        scriptMessageBridges = bridges
        for (index, name) in HandlerName.allCases.enumerated() {
            controller.add(bridges[index], name: name.rawValue)
        }
        webView.isOpaque = false
        webView.backgroundColor = .clear
        webView.scrollView.isScrollEnabled = false
        webView.scrollView.bounces = false
        webView.scrollView.delaysContentTouches = false
        webView.scrollView.canCancelContentTouches = false
        webView.scrollView.panGestureRecognizer.isEnabled = false
        webView.navigationDelegate = self
        loadTerminalPage()
    }

    deinit {
        let controller = userContentController
        let names = HandlerName.allCases.map(\.rawValue)
        if Thread.isMainThread {
            names.forEach { controller.removeScriptMessageHandler(forName: $0) }
        } else {
            DispatchQueue.main.async {
                names.forEach { controller.removeScriptMessageHandler(forName: $0) }
            }
        }
    }

    @MainActor
    func bindDisplayContainer(_ container: HtermTerminalContainerView) {
        displayContainer = container
    }

    @MainActor
    func unbindDisplayContainer(_ container: HtermTerminalContainerView) {
        if displayContainer === container {
            displayContainer = nil
        }
    }

    private func logTabInit(_ message: String) {
        tabInitLog("Hterm[\(instanceId)] \(message)")
    }

    private func pendingOutputSize() -> Int {
        outputLock.lock()
        let count = pendingOutput.count
        outputLock.unlock()
        return count
    }

    private func logFlushBlocked(_ reason: String) {
        if lastFlushBlockReason == reason {
            return
        }
        lastFlushBlockReason = reason
        logTabInit("flush blocked reason=\(reason) pending=\(pendingOutputSize()) loaded=\(isLoaded) hostReady=\(hostSizeReady)")
    }

    private func clearFlushBlocked() {
        guard lastFlushBlockReason != nil else { return }
        lastFlushBlockReason = nil
        logTabInit("flush unblocked pending=\(pendingOutputSize())")
    }

    func attach(to view: UIView) {
        webView.translatesAutoresizingMaskIntoConstraints = false
        view.addSubview(webView)
        NSLayoutConstraint.activate([
            webView.leadingAnchor.constraint(equalTo: view.leadingAnchor),
            webView.trailingAnchor.constraint(equalTo: view.trailingAnchor),
            webView.topAnchor.constraint(equalTo: view.topAnchor),
            webView.bottomAnchor.constraint(equalTo: view.bottomAnchor)
        ])
    }

    func enqueueOutput(_ data: Data) {
        guard !data.isEmpty else { return }
        outputLock.lock()
        pendingOutput.append(data)
        let shouldFlush = !outputInProgress
        if shouldFlush {
            outputInProgress = true
        }
        if Self.debugEnabled && !didLogFirstOutput {
            didLogFirstOutput = true
            NSLog("Hterm[%d]: first output enqueued (%lu bytes)", instanceId, data.count)
        }
        outputLock.unlock()
        if shouldFlush {
            DispatchQueue.main.async { [weak self] in
                self?.flushOutput()
            }
        }
    }

    func updateStyle(font: UIFont, foreground: UIColor, background: UIColor) {
        let payload: [String: Any] = [
            "foregroundColor": cssColor(from: foreground, fallback: "#FFFFFF"),
            "backgroundColor": cssColor(from: background, fallback: "#000000"),
            "fontFamily": font.familyName,
            "fontSize": font.pointSize,
            "blinkCursor": false,
            "cursorShape": "BLOCK"
        ]
        guard let data = try? JSONSerialization.data(withJSONObject: payload, options: []),
              let json = String(data: data, encoding: .utf8) else {
            return
        }
        let style = TerminalStylePayload(json: json)
        lastStyle = style
        if isLoaded {
            applyStyle(style)
        } else {
            pendingStyle = style
        }
    }

    func setFocused(_ focused: Bool) {
        lastFocus = focused
        if isLoaded {
            applyFocus(focused)
        } else {
            pendingFocus = focused
        }
    }

    func forceGridSize(columns: Int, rows: Int) {
        let clampedColumns = max(1, columns)
        let clampedRows = max(1, rows)
        sshResizeLog("[ssh-resize] hterm[\(instanceId)] force-grid req=\(columns)x\(rows) clamped=\(clampedColumns)x\(clampedRows) loaded=\(isLoaded)")
        pendingForcedGridSize = (clampedColumns, clampedRows)
        guard isLoaded else { return }
        applyForcedGridSize(columns: clampedColumns, rows: clampedRows)
    }

    func setResizeSessionId(_ sessionId: UInt64) {
        let previous = resizeSessionId
        resizeSessionId = sessionId
        sshResizeLog("[ssh-resize] hterm[\(instanceId)] bind-session previous=\(previous) session=\(sessionId)")
        guard sessionId != 0 else { return }
        if let pending = pendingRuntimeResize {
            pendingRuntimeResize = nil
            sshResizeLog("[ssh-resize] hterm[\(instanceId)] runtime-replay source=\(pending.source) session=\(sessionId) cols=\(pending.columns) rows=\(pending.rows)")
            PSCALRuntimeUpdateSessionWindowSize(sessionId, Int32(pending.columns), Int32(pending.rows))
        }
    }

    func userContentController(_ userContentController: WKUserContentController, didReceive message: WKScriptMessage) {
        switch message.name {
        case HandlerName.load.rawValue:
            let wasLoaded = isLoaded
            isLoaded = true
            logTabInit("load message wasLoaded=\(wasLoaded) hostReady=\(hostSizeReady) pending=\(pendingOutputSize())")
            if Self.debugEnabled {
                NSLog("Hterm[%d]: load message received", instanceId)
            }
            onLoaded?()
            if !wasLoaded {
                onLoadStateChange?(true)
            }
            if let style = pendingStyle {
                applyStyle(style)
                pendingStyle = nil
            } else if let style = lastStyle {
                applyStyle(style)
            }
            if let focus = pendingFocus {
                applyFocus(focus)
                pendingFocus = nil
            } else if let focus = lastFocus {
                applyFocus(focus)
            }
            flushOutput()
            requestResize()
            applyPendingForcedGridSizeIfNeeded()
        case HandlerName.log.rawValue:
            if let message = message.body as? String {
                NSLog("Hterm[%d] log: %@", instanceId, message)
            }
        case HandlerName.sendInput.rawValue:
            if let input = message.body as? String {
                onInput?(input)
            }
        case HandlerName.resize.rawValue:
            if !applyResizeMessage(message.body) {
                requestResize()
            }
        case HandlerName.propUpdate.rawValue:
            if let payload = message.body as? [Any], payload.count == 2,
               let name = payload[0] as? String {
                if name == "applicationCursor" {
                    let value = (payload[1] as? Bool) ?? (payload[1] as? NSNumber)?.boolValue ?? false
                    if applicationCursor != value {
                        applicationCursor = value
                        onApplicationCursorChange?(value)
                    }
                }
            }
        case HandlerName.focus.rawValue:
            onFocusRequested?()
        case HandlerName.syncFocus.rawValue:
            onSyncFocus?()
        case HandlerName.newScrollHeight.rawValue:
            if let value = message.body as? NSNumber {
                onScrollHeight?(CGFloat(value.doubleValue))
            } else if let value = message.body as? Double {
                onScrollHeight?(CGFloat(value))
            }
        case HandlerName.newScrollTop.rawValue:
            if let value = message.body as? NSNumber {
                onScrollTop?(CGFloat(value.doubleValue))
            } else if let value = message.body as? Double {
                onScrollTop?(CGFloat(value))
            }
        case HandlerName.openLink.rawValue:
            if let urlString = message.body as? String, let url = URL(string: urlString) {
                UIApplication.shared.open(url)
            }
        case HandlerName.selectionChanged.rawValue:
            if message.body is NSNull {
                displayContainer?.updateSelectionMenu(rect: nil)
                break
            }
            if let payload = message.body as? [String: Any] {
                func number(_ value: Any?) -> CGFloat? {
                    if let number = value as? NSNumber {
                        return CGFloat(number.doubleValue)
                    }
                    if let value = value as? Double {
                        return CGFloat(value)
                    }
                    return nil
                }
                if let x = number(payload["x"]),
                   let y = number(payload["y"]),
                   let width = number(payload["width"]),
                   let height = number(payload["height"]) {
                    displayContainer?.updateSelectionMenu(rect: CGRect(x: x, y: y, width: width, height: height))
                } else {
                    displayContainer?.updateSelectionMenu(rect: nil)
                }
            } else {
                displayContainer?.updateSelectionMenu(rect: nil)
            }
        default:
            break
        }
    }

    private func loadTerminalPage() {
        guard let url = terminalPageURL() else {
            NSLog("Hterm error: terminal page URL not found in bundle")
            return
        }
        logTabInit("load page url=\(url.lastPathComponent)")
        if Self.debugEnabled {
            NSLog("Hterm[%d]: loading terminal page %@", instanceId, url.absoluteString)
        }
        webView.load(URLRequest(url: url))
    }

    private func terminalPageURL() -> URL? {
        URL(string: "\(Self.terminalScheme)://terminal/term.html")
    }

    private func requestResize(after delay: TimeInterval = 0) {
        resizeRequestGeneration &+= 1
        let generation = resizeRequestGeneration
        sshResizeLog("[ssh-resize] hterm[\(instanceId)] request-resize gen=\(generation) delay=\(String(format: "%.3f", delay)) loaded=\(isLoaded)")
        let evaluate: () -> Void = { [weak self] in
            guard let self = self else { return }
            self.webView.evaluateJavaScript("exports.getSize()") { [weak self] result, error in
                guard let self = self else { return }
                if let error = error {
                    NSLog("Hterm resize error: %@", error.localizedDescription)
                    sshResizeLog("[ssh-resize] hterm[\(self.instanceId)] request-resize error=\(error.localizedDescription)")
                }
                guard generation == self.resizeRequestGeneration else { return }
                guard let array = result as? [NSNumber], array.count == 2 else { return }
                sshResizeLog("[ssh-resize] hterm[\(self.instanceId)] request-resize result=\(array[0].intValue)x\(array[1].intValue) gen=\(generation)")
                self.forwardResizeToRuntime(columns: array[0].intValue, rows: array[1].intValue, source: "request")
                self.onResize?(array[0].intValue, array[1].intValue)
            }
        }
        if delay > 0 {
            DispatchQueue.main.asyncAfter(deadline: .now() + delay, execute: evaluate)
        } else {
            evaluate()
        }
    }

    private func applyResizeMessage(_ body: Any) -> Bool {
        func parsePair(_ columns: Int, _ rows: Int) -> Bool {
            guard columns > 0, rows > 0 else { return false }
            sshResizeLog("[ssh-resize] hterm[\(instanceId)] native-resize=\(columns)x\(rows)")
            forwardResizeToRuntime(columns: columns, rows: rows, source: "native")
            onResize?(columns, rows)
            return true
        }
        if let payload = body as? [String: Any] {
            if let cols = payload["columns"] as? NSNumber,
               let rows = payload["rows"] as? NSNumber {
                return parsePair(cols.intValue, rows.intValue)
            }
            if let cols = payload["columns"] as? Int,
               let rows = payload["rows"] as? Int {
                return parsePair(cols, rows)
            }
            return false
        }
        if let payload = body as? [NSNumber], payload.count >= 2 {
            return parsePair(payload[0].intValue, payload[1].intValue)
        }
        if let payload = body as? [Int], payload.count >= 2 {
            return parsePair(payload[0], payload[1])
        }
        return false
    }

    private func forwardResizeToRuntime(columns: Int, rows: Int, source: String) {
        let sessionId = resizeSessionId
        guard sessionId != 0 else {
            pendingRuntimeResize = (columns, rows, source)
            sshResizeLog("[ssh-resize] hterm[\(instanceId)] runtime-defer source=\(source) session=0 cols=\(columns) rows=\(rows)")
            return
        }
        pendingRuntimeResize = nil
        sshResizeLog("[ssh-resize] hterm[\(instanceId)] runtime-forward source=\(source) session=\(sessionId) cols=\(columns) rows=\(rows)")
        PSCALRuntimeUpdateSessionWindowSize(sessionId, Int32(columns), Int32(rows))
    }

    func updateHostSize(_ size: CGSize, reason: String) {
        var clampedSize = size
        var didClamp = false
        if clampedSize.width <= 0 {
            clampedSize.width = 1
            didClamp = true
        }
        if clampedSize.height <= 0 {
            clampedSize.height = 1
            didClamp = true
        }
        if didClamp, !didLogZeroHostSize {
            let sizeString = NSCoder.string(for: size)
            logTabInit("host size clamped from \(sizeString) reason=\(reason)")
            didLogZeroHostSize = true
        } else if didLogZeroHostSize && !didClamp {
            didLogZeroHostSize = false
        }
        if didLogZeroHostSize {
            didLogZeroHostSize = false
        }
        let wasReady = hostSizeReady
        let changed = clampedSize != hostSize
        hostSize = clampedSize
        hostSizeReady = true
        if !wasReady || changed {
            let sizeString = NSCoder.string(for: clampedSize)
            logTabInit("host size ready=\(hostSizeReady) size=\(sizeString) reason=\(reason) loaded=\(isLoaded)")
        }
        if Self.debugEnabled && (!wasReady || changed) {
            let sizeString = NSCoder.string(for: clampedSize)
            NSLog("Hterm[%d]: host size %@ (%@)", instanceId, sizeString, reason)
        }
        if isLoaded && (!wasReady || changed) {
            requestResize()
        }
        var shouldFlush = false
        outputLock.lock()
        if isLoaded && hostSizeReady && !pendingOutput.isEmpty && !outputInProgress {
            outputInProgress = true
            shouldFlush = true
        }
        outputLock.unlock()
        if shouldFlush {
            DispatchQueue.main.async { [weak self] in
                self?.flushOutput()
            }
        }
    }

    private func flushOutput() {
        guard isLoaded else {
            logFlushBlocked("not-loaded")
            outputLock.lock()
            outputInProgress = false
            outputLock.unlock()
            return
        }
        if !hostSizeReady {
            logFlushBlocked("no-host-size")
            outputLock.lock()
            outputInProgress = false
            outputLock.unlock()
            return
        }
        clearFlushBlocked()
        let chunk: Data
        outputLock.lock()
        if pendingOutput.isEmpty {
            outputInProgress = false
            outputLock.unlock()
            return
        }
        let amount = min(pendingOutput.count, maxFlushBytes)
        chunk = pendingOutput.prefix(amount)
        pendingOutput.removeFirst(amount)
        if Self.debugEnabled && !didLogFirstFlush {
            didLogFirstFlush = true
            NSLog("Hterm[%d]: flushing first output chunk (%lu bytes)", instanceId, chunk.count)
        }
        outputLock.unlock()

        guard let js = jsWriteString(for: chunk) else {
            scheduleNextFlushIfNeeded()
            return
        }

        webView.evaluateJavaScript(js) { [weak self] _, error in
            if let error = error {
                NSLog("Hterm output error: %@", error.localizedDescription)
            }
            self?.scheduleNextFlushIfNeeded()
        }
    }

    private func scheduleNextFlushIfNeeded() {
        outputLock.lock()
        let hasMore = !pendingOutput.isEmpty
        if !hasMore {
            outputInProgress = false
        }
        outputLock.unlock()
        if hasMore {
            DispatchQueue.main.async { [weak self] in
                self?.flushOutput()
            }
        }
    }

    private func applyStyle(_ style: TerminalStylePayload) {
        let script = "exports.updateStyle(\(style.json))"
        webView.evaluateJavaScript(script) { [weak self] _, error in
            if let error = error {
                NSLog("Hterm style error: %@", error.localizedDescription)
            }
            self?.requestResize(after: 0.06)
            self?.applyPendingForcedGridSizeIfNeeded()
        }
    }

    private func applyPendingForcedGridSizeIfNeeded() {
        guard isLoaded, let pending = pendingForcedGridSize else {
            return
        }
        applyForcedGridSize(columns: pending.columns, rows: pending.rows)
    }

    private func applyForcedGridSize(columns: Int, rows: Int) {
        guard columns > 0, rows > 0 else { return }
        let script = "exports.setGridSize(\(columns), \(rows))"
        webView.evaluateJavaScript(script) { _, error in
            if let error = error {
                NSLog("Hterm force size error: %@", error.localizedDescription)
                sshResizeLog("[ssh-resize] hterm[\(self.instanceId)] force-grid error=\(error.localizedDescription) cols=\(columns) rows=\(rows)")
            } else {
                sshResizeLog("[ssh-resize] hterm[\(self.instanceId)] force-grid applied=\(columns)x\(rows)")
            }
        }
    }

    private func applyFocus(_ focused: Bool) {
        let script = "exports.setFocused(\(focused ? "true" : "false"))"
        webView.evaluateJavaScript(script, completionHandler: nil)
    }

    func setScrollTop(_ value: CGFloat) {
        let script = String(format: "exports.newScrollTop(%f)", value)
        webView.evaluateJavaScript(script, completionHandler: nil)
    }

    func notifyUserInput() {
        guard isLoaded else { return }
        webView.evaluateJavaScript("exports.setUserGesture()", completionHandler: nil)
        scheduleScrollToBottom()
    }

    func copySelectionToClipboard() {
        guard isLoaded else { return }
        webView.evaluateJavaScript("exports.copy()", completionHandler: nil)
    }

    private func jsWriteString(for data: Data) -> String? {
        let encoded = data.base64EncodedString()
        return "exports.writeB64(\"\(encoded)\")"
    }

    private func scheduleScrollToBottom() {
        guard !scrollToBottomPending else { return }
        scrollToBottomPending = true
        DispatchQueue.main.async { [weak self] in
            guard let self else { return }
            self.scrollToBottomPending = false
            guard self.isLoaded else { return }
            self.webView.evaluateJavaScript("exports.scrollToBottom()", completionHandler: nil)
        }
    }

    private func cssColor(from color: UIColor, fallback: String) -> String {
        let resolved = color.resolvedColor(with: webView.traitCollection)
        var red: CGFloat = 0
        var green: CGFloat = 0
        var blue: CGFloat = 0
        var alpha: CGFloat = 0
        if resolved.getRed(&red, green: &green, blue: &blue, alpha: &alpha) {
            let r = Int(red * 255.0)
            let g = Int(green * 255.0)
            let b = Int(blue * 255.0)
            return String(format: "#%02X%02X%02X", r, g, b)
        }
        var white: CGFloat = 0
        if resolved.getWhite(&white, alpha: &alpha) {
            let value = Int(white * 255.0)
            return String(format: "#%02X%02X%02X", value, value, value)
        }
        if let components = resolved.cgColor.components {
            if components.count >= 3 {
                let r = Int(components[0] * 255.0)
                let g = Int(components[1] * 255.0)
                let b = Int(components[2] * 255.0)
                return String(format: "#%02X%02X%02X", r, g, b)
            }
            if components.count == 2 {
                let value = Int(components[0] * 255.0)
                return String(format: "#%02X%02X%02X", value, value, value)
            }
        }
        return fallback
    }

    private func handleWebContentReset(reason: String, error: Error? = nil) {
        if let error {
            logTabInit("web content reset reason=\(reason) error=\(error.localizedDescription)")
        } else {
            logTabInit("web content reset reason=\(reason)")
        }
        if let error = error {
            NSLog("Hterm web content reset (%@): %@", reason, error.localizedDescription)
        } else {
            NSLog("Hterm web content reset (%@)", reason)
        }
        let wasLoaded = isLoaded
        isLoaded = false
        outputLock.lock()
        outputInProgress = false
        outputLock.unlock()
        if let style = lastStyle {
            pendingStyle = style
        }
        if let focus = lastFocus {
            pendingFocus = focus
        }
        if wasLoaded {
            onLoadStateChange?(false)
        }
        scheduleReload()
    }

    private func scheduleReload() {
        guard !reloadPending else { return }
        reloadPending = true
        logTabInit("reload scheduled")
        DispatchQueue.main.asyncAfter(deadline: .now() + 1.15) { [weak self] in
            guard let self else { return }
            self.reloadPending = false
            self.loadTerminalPage()
        }
    }

    func webViewWebContentProcessDidTerminate(_ webView: WKWebView) {
        handleWebContentReset(reason: "webcontent-terminated")
    }

    func webView(_ webView: WKWebView, didFail navigation: WKNavigation!, withError error: Error) {
        if shouldIgnoreNavigationError(error) {
            return
        }
        handleWebContentReset(reason: "navigation-failed", error: error)
    }

    func webView(_ webView: WKWebView, didFailProvisionalNavigation navigation: WKNavigation!, withError error: Error) {
        if shouldIgnoreNavigationError(error) {
            return
        }
        handleWebContentReset(reason: "provisional-navigation-failed", error: error)
    }

    private func shouldIgnoreNavigationError(_ error: Error) -> Bool {
        let nsError = error as NSError
        if nsError.domain == NSURLErrorDomain && nsError.code == NSURLErrorCancelled {
            return true
        }
        if nsError.domain == WKErrorDomain && nsError.code == WKError.webContentProcessTerminated.rawValue {
            return true
        }
        return false
    }
}

private final class WeakScriptMessageHandler: NSObject, WKScriptMessageHandler {
    weak var handler: WKScriptMessageHandler?

    init(handler: WKScriptMessageHandler) {
        self.handler = handler
    }

    func userContentController(_ userContentController: WKUserContentController, didReceive message: WKScriptMessage) {
        handler?.userContentController(userContentController, didReceive: message)
    }
}

private final class ScrollbarViewDelegateProxy: NSObject, UIScrollViewDelegate {
    weak var innerDelegate: UIScrollViewDelegate?

    override func responds(to aSelector: Selector!) -> Bool {
        if super.responds(to: aSelector) {
            return true
        }
        return innerDelegate?.responds(to: aSelector) ?? false
    }

    override func forwardingTarget(for aSelector: Selector!) -> Any? {
        if innerDelegate?.responds(to: aSelector) == true {
            return innerDelegate
        }
        return super.forwardingTarget(for: aSelector)
    }

    func scrollViewDidScroll(_ scrollView: UIScrollView) {
        guard let scrollbarView = scrollView as? ScrollbarView,
              let contentView = scrollbarView.contentView else {
            innerDelegate?.scrollViewDidScroll?(scrollView)
            return
        }

        scrollbarView.syncContentViewPosition()

        innerDelegate?.scrollViewDidScroll?(scrollView)
    }
}

private final class ScrollbarView: UIScrollView {
    fileprivate let outerDelegate = ScrollbarViewDelegateProxy()
    fileprivate var contentViewOrigin: CGPoint = .zero

    var contentView: UIView? {
        didSet {
            contentViewOrigin = contentView?.frame.origin ?? .zero
        }
    }

    fileprivate func syncContentViewPosition() {
        guard let contentView else { return }
        var frame = contentView.frame
        frame.origin.x = contentOffset.x + contentViewOrigin.x
        frame.origin.y = contentOffset.y + contentViewOrigin.y
        contentView.frame = frame
    }

    override init(frame: CGRect) {
        super.init(frame: frame)
        super.delegate = outerDelegate
    }

    @available(*, unavailable)
    required init?(coder: NSCoder) {
        fatalError("init(coder:) has not been implemented")
    }

    override var delegate: UIScrollViewDelegate? {
        get { outerDelegate.innerDelegate }
        set { outerDelegate.innerDelegate = newValue }
    }
}

@MainActor
final class HtermTerminalContainerView: UIView, UIScrollViewDelegate {
    private let controller: HtermTerminalController
    private let scrollView: ScrollbarView
    private let keyInputView: TerminalKeyInputView
    private var focusRetry: DispatchWorkItem?
    private var pendingFocus = false
    private var isAttached = false
    private var isActiveForInput = true
    private var lastVisibilityForInput: Bool?
    private var attachHandler: (() -> Void)?
    private var detachHandler: (() -> Void)?
    private var windowObservers: [NSObjectProtocol] = []
    private var isSyncingScroll = false
    private var isTerminalInstalled = false
    private var isTerminalLoaded = false
    private var selectionMenuVisible = false
    private var lastSelectionRect: CGRect?

    private func debugLog(_ message: String) {
        guard HtermTerminalController.debugEnabled else { return }
        if let data = (message + "\n").data(using: .utf8) {
            FileHandle.standardError.write(data)
        }
    }

    private func logTabInit(_ message: String) {
        tabInitLog("HtermView[\(controller.instanceId)] \(message)")
    }

    private func isDisplayVisible() -> Bool {
        guard isEffectivelyVisible() else { return false }
        return window?.isKeyWindow ?? false
    }

    private func updateDisplayAttachment() {
        let loaded = controller.isLoaded
        if isTerminalLoaded != loaded {
            isTerminalLoaded = loaded
            updateInputEnabled()
        }
        let shouldInstall = isTerminalLoaded && isDisplayVisible()
        if shouldInstall {
            controller.bindDisplayContainer(self)
            installTerminalViewIfNeeded()
            if pendingFocus {
                requestFocus()
            }
        } else {
            controller.unbindDisplayContainer(self)
            uninstallTerminalViewIfNeeded()
        }
    }

    init(controller: HtermTerminalController) {
        self.controller = controller
        self.scrollView = ScrollbarView()
        self.keyInputView = TerminalKeyInputView()
        super.init(frame: .zero)
        isOpaque = false
        configureScrollView()
        configureKeyInputView()
        updateInputEnabled()
        let tap = UITapGestureRecognizer(target: self, action: #selector(handleTap))
        tap.cancelsTouchesInView = false
        addGestureRecognizer(tap)
        installWindowObservers()
        logTabInit("container init")
    }

    @available(*, unavailable)
    required init?(coder: NSCoder) {
        fatalError("init(coder:) has not been implemented")
    }

    deinit {
        windowObservers.forEach { NotificationCenter.default.removeObserver($0) }
        if isAttached {
            detachHandler?()
        }
    }

    func updateInputHandlers(
        onPaste: ((String) -> Void)?,
        onInterrupt: (() -> Void)?,
        onSuspend: (() -> Void)?,
        onCloseTab: (() -> Void)?
    ) {
        keyInputView.onInput = { [weak controller] text in
            controller?.onInput?(text)
        }
        keyInputView.onPaste = { [weak controller] text in
            onPaste?(text)
            controller?.notifyUserInput()
        }
        keyInputView.onCopy = { [weak controller] in
            controller?.copySelectionToClipboard()
        }
        keyInputView.onInterrupt = onInterrupt
        keyInputView.onSuspend = onSuspend
        keyInputView.onCloseTab = onCloseTab
        keyInputView.onNewTab = {
            _ = TerminalTabManager.shared.openShellTab()
        }
        if let webView = controller.webView as? HtermWebView {
            webView.onPaste = { [weak controller] text in
                onPaste?(text)
                controller?.notifyUserInput()
            }
            webView.onCopy = { [weak controller] in
                controller?.copySelectionToClipboard()
            }
        }
    }

    func requestFocus() {
        guard canReceiveInputFocus() else {
            pendingFocus = true
            return
        }
        if keyInputView.isFirstResponder {
            pendingFocus = false
            focusRetry?.cancel()
            return
        }
        let became = keyInputView.becomeFirstResponder()
        if became {
            pendingFocus = false
            focusRetry?.cancel()
            return
        }
        pendingFocus = true
        focusRetry?.cancel()
        let work = DispatchWorkItem { [weak self] in
            guard let self else { return }
            if self.pendingFocus {
                self.requestFocus()
            }
        }
        focusRetry = work
        DispatchQueue.main.asyncAfter(deadline: .now() + 1.05, execute: work)
    }

    func syncFocusFromTerminal() {
        updateFocusForKeyWindow()
    }

    func updateScrollHeight(_ height: CGFloat) {
        let width = scrollView.bounds.width > 0 ? scrollView.bounds.width : max(scrollView.contentSize.width, 1)
        scrollView.contentSize = CGSize(width: width, height: height)
        clampScrollOffset(reason: "height")
        if HtermTerminalController.debugEnabled {
            debugLog("Hterm[\(controller.instanceId)]: scroll height=\(String(format: "%.2f", height)) " +
                     "contentOffset=\(NSCoder.string(for: scrollView.contentOffset)) " +
                     "bounds=\(NSCoder.string(for: scrollView.bounds))")
        }
    }

    func updateScrollTop(_ top: CGFloat) {
        let maxTop = max(0, scrollView.contentSize.height - scrollView.bounds.height)
        let clampedTop = min(max(top, 0), maxTop)
        if abs(scrollView.contentOffset.y - clampedTop) < 0.5 {
            return
        }
        isSyncingScroll = true
        scrollView.setContentOffset(CGPoint(x: 0, y: clampedTop), animated: false)
        isSyncingScroll = false
        if HtermTerminalController.debugEnabled {
            debugLog("Hterm[\(controller.instanceId)]: scroll top=\(String(format: "%.2f", clampedTop)) " +
                     "contentOffset=\(NSCoder.string(for: scrollView.contentOffset)) " +
                     "contentSize=\(NSCoder.string(for: scrollView.contentSize))")
        }
    }

    private func clampScrollOffset(reason: String) {
        let maxX = max(0, scrollView.contentSize.width - scrollView.bounds.width)
        let maxY = max(0, scrollView.contentSize.height - scrollView.bounds.height)
        let current = scrollView.contentOffset
        let clamped = CGPoint(
            x: min(max(current.x, 0), maxX),
            y: min(max(current.y, 0), maxY)
        )
        if abs(current.x - clamped.x) < 0.5 && abs(current.y - clamped.y) < 0.5 {
            scrollView.syncContentViewPosition()
            return
        }
        isSyncingScroll = true
        scrollView.setContentOffset(clamped, animated: false)
        isSyncingScroll = false
        scrollView.syncContentViewPosition()
        if HtermTerminalController.debugEnabled {
            debugLog("Hterm[\(controller.instanceId)]: scroll clamp reason=\(reason) " +
                     "contentOffset=\(NSCoder.string(for: scrollView.contentOffset)) " +
                     "bounds=\(NSCoder.string(for: scrollView.bounds)) " +
                     "contentSize=\(NSCoder.string(for: scrollView.contentSize))")
        }
    }

    func updateSelectionMenu(rect: CGRect?) {
        guard let webView = controller.webView as? HtermWebView else { return }
        DispatchQueue.main.async { [weak self] in
            guard let self else { return }
            if let rect = rect, !rect.isNull {
                let menuVisible = UIMenuController.shared.isMenuVisible
                if let last = lastSelectionRect,
                   rect.equalTo(last),
                   selectionMenuVisible,
                   menuVisible {
                    return
                }
                if keyInputView.isFirstResponder {
                    keyInputView.resignFirstResponder()
                }
                _ = webView.becomeFirstResponder()
                let target = rect.insetBy(dx: -4, dy: -4)
                let clamped = target.intersection(webView.bounds)
                let effectiveTarget = clamped.isNull ? target : clamped
                UIMenuController.shared.setTargetRect(effectiveTarget, in: webView)
                UIMenuController.shared.setMenuVisible(true, animated: true)
                selectionMenuVisible = true
                lastSelectionRect = rect
            } else if selectionMenuVisible {
                UIMenuController.shared.setMenuVisible(false, animated: true)
                selectionMenuVisible = false
                lastSelectionRect = nil
                if isActiveForInput {
                    keyInputView.becomeFirstResponder()
                }
            }
        }
    }

    func updateApplicationCursor(_ enabled: Bool) {
        keyInputView.applicationCursorEnabled = enabled
    }

    func updateAttachHandlers(onAttach: (() -> Void)?, onDetach: (() -> Void)?) {
        attachHandler = onAttach
        detachHandler = onDetach
    }

    func updateTerminalLoaded(_ loaded: Bool) {
        if isTerminalLoaded != loaded {
            let keyWindow = window?.isKeyWindow ?? false
            logTabInit("terminal loaded=\(loaded) active=\(isActiveForInput) attached=\(isAttached) window=\(window != nil) key=\(keyWindow)")
        }
        isTerminalLoaded = loaded
        updateInputEnabled()
        if loaded {
            updateDisplayAttachment()
            controller.updateHostSize(scrollView.bounds.size, reason: "load")
            if pendingFocus {
                requestFocus()
            }
        } else {
            updateDisplayAttachment()
        }
        updateVisibilityForInput()
    }

    func setActive(_ active: Bool) {
        if isActiveForInput == active {
            updateInputEnabled()
            return
        }
        isActiveForInput = active
        let keyWindow = window?.isKeyWindow ?? false
        logTabInit("active=\(active) loaded=\(isTerminalLoaded) attached=\(isAttached) window=\(window != nil) key=\(keyWindow)")
        updateInputEnabled()
        if !active {
            pendingFocus = false
            controller.setFocused(false)
            if keyInputView.isFirstResponder {
                keyInputView.resignFirstResponder()
            }
            updateDisplayAttachment()
        } else if controller.isLoaded {
            updateDisplayAttachment()
            controller.updateHostSize(scrollView.bounds.size, reason: "active")
        }
        handleAttachStateChange()
        updateVisibilityForInput()
    }

    override func didMoveToWindow() {
        super.didMoveToWindow()
        handleAttachStateChange()
        updateFocusForKeyWindow()
        updateVisibilityForInput()
        updateDisplayAttachment()
        if window != nil, pendingFocus {
            pendingFocus = false
            requestFocus()
        }
    }

    override func layoutSubviews() {
        super.layoutSubviews()
        if controller.webView.superview === scrollView {
            var frame = controller.webView.frame
            frame.size = scrollView.bounds.size
            controller.webView.frame = frame
            scrollView.syncContentViewPosition()
            if HtermTerminalController.debugEnabled {
                debugLog("Hterm[\(controller.instanceId)]: layout bounds=\(NSCoder.string(for: scrollView.bounds)) " +
                         "webView=\(NSCoder.string(for: controller.webView.frame)) " +
                         "contentOffset=\(NSCoder.string(for: scrollView.contentOffset))")
            }
        }
        controller.updateHostSize(scrollView.bounds.size, reason: "layout")
        clampScrollOffset(reason: "layout")
        updateVisibilityForInput()
    }

    private func installWindowObservers() {
        let center = NotificationCenter.default
        let becameKey = center.addObserver(
            forName: UIWindow.didBecomeKeyNotification,
            object: nil,
            queue: .main
        ) { [weak self] notification in
            Task { @MainActor in
                self?.handleWindowNotification(notification)
            }
        }
        let resignedKey = center.addObserver(
            forName: UIWindow.didResignKeyNotification,
            object: nil,
            queue: .main
        ) { [weak self] notification in
            Task { @MainActor in
                self?.handleWindowNotification(notification)
            }
        }
        windowObservers = [becameKey, resignedKey]
    }

    private func handleWindowNotification(_ notification: Notification) {
        guard let window = notification.object as? UIWindow else { return }
        guard window === self.window else { return }
        if !window.isKeyWindow && isAppActive() {
            return
        }
        updateFocusForKeyWindow()
        updateVisibilityForInput()
        updateDisplayAttachment()
    }

    private func configureScrollView() {
        scrollView.delegate = self
        scrollView.bounces = false
        scrollView.alwaysBounceVertical = false
        scrollView.showsVerticalScrollIndicator = true
        scrollView.showsHorizontalScrollIndicator = false
        scrollView.delaysContentTouches = false
        scrollView.canCancelContentTouches = false
        scrollView.isOpaque = false
        scrollView.backgroundColor = .clear
        scrollView.clipsToBounds = true
        if #available(iOS 11.0, *) {
            scrollView.contentInsetAdjustmentBehavior = .never
        }
        scrollView.autoresizingMask = [.flexibleWidth, .flexibleHeight]
        scrollView.frame = bounds
        addSubview(scrollView)
    }

    func scrollViewDidScroll(_ scrollView: UIScrollView) {
        if !isSyncingScroll {
            controller.setScrollTop(scrollView.contentOffset.y)
        }
    }

    private func installTerminalViewIfNeeded() {
        guard !isTerminalInstalled else { return }
        if controller.webView.superview != nil {
            controller.webView.removeFromSuperview()
        }
        controller.webView.frame = scrollView.bounds
        controller.webView.autoresizingMask = [.flexibleWidth, .flexibleHeight]
        controller.webView.isOpaque = false
        controller.webView.backgroundColor = .clear
        controller.webView.scrollView.isScrollEnabled = false
        controller.webView.scrollView.bounces = false
        controller.webView.scrollView.delaysContentTouches = false
        controller.webView.scrollView.canCancelContentTouches = false
        controller.webView.scrollView.panGestureRecognizer.isEnabled = false
        scrollView.contentView = controller.webView
        scrollView.addSubview(controller.webView)
        scrollView.syncContentViewPosition()
        isTerminalInstalled = true
        logTabInit("install web view loaded=\(controller.isLoaded)")
        debugLog("Hterm[\(controller.instanceId)]: install web view (loaded=\(controller.isLoaded))")
        if isEffectivelyVisible() {
            controller.updateHostSize(scrollView.bounds.size, reason: "install")
        }
    }

    private func uninstallTerminalViewIfNeeded() {
        guard isTerminalInstalled else { return }
        if controller.webView.superview === scrollView {
            controller.webView.removeFromSuperview()
        }
        scrollView.contentView = nil
        isTerminalInstalled = false
        logTabInit("uninstall web view loaded=\(controller.isLoaded)")
        debugLog("Hterm[\(controller.instanceId)]: uninstall web view (loaded=\(controller.isLoaded))")
    }

    private func handleAttachStateChange() {
        let shouldAttach = (window != nil)
        if shouldAttach {
            if isAttached {
                return
            }
            isAttached = true
            logTabInit("view attached window=\(String(describing: window)) hidden=\(isHidden) alpha=\(alpha)")
            debugLog("Hterm[\(controller.instanceId)]: view attached " +
                     "window=\(String(describing: window)) hidden=\(isHidden) alpha=\(alpha)")
            attachHandler?()
            if pendingFocus {
                requestFocus()
            }
            return
        }
        if !isAttached {
            debugLog("Hterm[\(controller.instanceId)]: view detached ignored " +
                     "window=\(String(describing: window)) superview=\(String(describing: superview)) " +
                     "hidden=\(isHidden) alpha=\(alpha)")
            return
        }
        isAttached = false
        logTabInit("view detached window=\(String(describing: window)) superview=\(String(describing: superview)) hidden=\(isHidden) alpha=\(alpha)")
        debugLog("Hterm[\(controller.instanceId)]: view detached " +
                 "window=\(String(describing: window)) superview=\(String(describing: superview)) " +
                 "hidden=\(isHidden) alpha=\(alpha)")
        detachHandler?()
    }

    private func updateFocusForKeyWindow() {
        guard window != nil else { return }
        let focused = (window?.isKeyWindow ?? false) && isEffectivelyVisible()
        controller.setFocused(focused)
        if focused {
            updateVisibilityForInput()
        } else if keyInputView.isFirstResponder {
            keyInputView.resignFirstResponder()
        }
    }

    private func canReceiveInputFocus() -> Bool {
        guard isActiveForInput else { return false }
        guard window != nil else { return false }
        if !isAppActive() {
            return false
        }
        if let window, window.isKeyWindow {
            return isEffectivelyVisible()
        }
        /* On iPad we can attach before the window is key; allow focus to be
         * requested and retried so the keyboard appears without an extra tap. */
        return isEffectivelyVisible()
    }

    private func isEffectivelyVisible() -> Bool {
        if !isActiveForInput || window == nil {
            return false
        }
        return true
    }

    private func isAppActive() -> Bool {
        UIApplication.shared.applicationState == .active
    }

    private func updateVisibilityForInput() {
        let visible = isDisplayVisible()
        if visible == lastVisibilityForInput {
            return
        }
        lastVisibilityForInput = visible
        if !visible {
            if keyInputView.isFirstResponder {
                keyInputView.resignFirstResponder()
            }
            updateDisplayAttachment()
            return
        }
        updateDisplayAttachment()
        if pendingFocus {
            pendingFocus = false
            requestFocus()
        }
    }

    private func updateInputEnabled() {
        let enabled = isActiveForInput
        if keyInputView.inputEnabled != enabled {
            keyInputView.inputEnabled = enabled
        }
    }

    @objc private func handleTap() {
        requestFocus()
    }

    private func configureKeyInputView() {
        keyInputView.backgroundColor = .clear
        keyInputView.textColor = .clear
        keyInputView.tintColor = .clear
        keyInputView.isScrollEnabled = false
        keyInputView.text = ""
        keyInputView.inputEnabled = false
        keyInputView.autocorrectionType = .no
        keyInputView.autocapitalizationType = .none
        keyInputView.spellCheckingType = .no
        keyInputView.smartQuotesType = .no
        keyInputView.smartInsertDeleteType = .no
        keyInputView.smartDashesType = .no
        keyInputView.keyboardAppearance = .dark
        keyInputView.onFocusChange = { [weak self] focused in
            self?.controller.setFocused(focused)
        }
        keyInputView.inputAssistantItem.leadingBarButtonGroups = []
        keyInputView.inputAssistantItem.trailingBarButtonGroups = []

        keyInputView.translatesAutoresizingMaskIntoConstraints = false
        addSubview(keyInputView)
        NSLayoutConstraint.activate([
            keyInputView.leadingAnchor.constraint(equalTo: leadingAnchor),
            keyInputView.bottomAnchor.constraint(equalTo: bottomAnchor),
            keyInputView.widthAnchor.constraint(equalToConstant: 1),
            keyInputView.heightAnchor.constraint(equalToConstant: 1)
        ])
    }
}

struct HtermTerminalView: UIViewRepresentable {
    let controller: HtermTerminalController
    let font: UIFont
    let foregroundColor: UIColor
    let backgroundColor: UIColor
    var focusToken: Int = 0
    var isActive: Bool = true
    var onInput: ((String) -> Void)?
    var onPaste: ((String) -> Void)?
    var onInterrupt: (() -> Void)?
    var onSuspend: (() -> Void)?
    var onResize: ((Int, Int) -> Void)?
    var onReady: ((HtermTerminalController) -> Void)?
    var onDetach: ((HtermTerminalController) -> Void)? = nil
    var onLoadStateChange: ((Bool) -> Void)? = nil

    func makeCoordinator() -> Coordinator {
        Coordinator(controller: controller)
    }

    func makeUIView(context: Context) -> HtermTerminalContainerView {
        tabInitLog("HtermView makeUIView id=\(controller.instanceId) loaded=\(controller.isLoaded)")
        let container = HtermTerminalContainerView(controller: context.coordinator.controller)
        container.backgroundColor = backgroundColor
        configure(container, context: context)
        return container
    }

    func updateUIView(_ uiView: HtermTerminalContainerView, context: Context) {
        uiView.backgroundColor = backgroundColor
        configure(uiView, context: context)
    }

    private func configure(
        _ uiView: HtermTerminalContainerView,
        context: Context
    ) {
        let controller = context.coordinator.controller
        if context.coordinator.lastActive != isActive {
            tabInitLog("HtermView update id=\(controller.instanceId) active=\(isActive) loaded=\(controller.isLoaded)")
            context.coordinator.lastActive = isActive
        }
        controller.onInput = { [weak controller] input in
            onInput?(input)
            controller?.notifyUserInput()
        }
        controller.onResize = onResize
        controller.onFocusRequested = { [weak controller] in
            controller?.displayContainer?.requestFocus()
        }
        controller.onSyncFocus = { [weak controller] in
            controller?.displayContainer?.syncFocusFromTerminal()
        }
        controller.onScrollHeight = { [weak controller] height in
            controller?.displayContainer?.updateScrollHeight(height)
        }
        controller.onScrollTop = { [weak controller] top in
            controller?.displayContainer?.updateScrollTop(top)
        }
        controller.onApplicationCursorChange = { [weak controller] enabled in
            controller?.displayContainer?.updateApplicationCursor(enabled)
        }
        controller.onLoadStateChange = { [weak controller] loaded in
            controller?.displayContainer?.updateTerminalLoaded(loaded)
            onLoadStateChange?(loaded)
        }

        uiView.updateInputHandlers(
            onPaste: onPaste,
            onInterrupt: onInterrupt,
            onSuspend: onSuspend,
            onCloseTab: {
                TerminalTabManager.shared.closeSelectedTab()
            }
        )

        uiView.setActive(isActive)
        controller.updateStyle(font: font, foreground: foregroundColor, background: backgroundColor)

        if context.coordinator.focusToken != focusToken {
            context.coordinator.focusToken = focusToken
            tabInitLog("HtermView focus request id=\(controller.instanceId) token=\(focusToken)")
            uiView.requestFocus()
        }

        uiView.updateAttachHandlers(
            onAttach: { onReady?(controller) },
            onDetach: { onDetach?(controller) }
        )
        uiView.updateTerminalLoaded(controller.isLoaded)
    }

    final class Coordinator {
        let controller: HtermTerminalController
        var focusToken: Int = 0
        var lastActive: Bool?

        init(controller: HtermTerminalController) {
            self.controller = controller
        }
    }
}
