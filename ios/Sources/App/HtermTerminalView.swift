import SwiftUI
import WebKit
import UIKit

final class HtermTerminalController: NSObject, WKScriptMessageHandler {
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
    }

    private struct TerminalStylePayload {
        let json: String
    }

    private let outputLock = NSLock()
    private var pendingOutput = Data()
    private var outputInProgress = false
    private let maxFlushBytes = 32 * 1024

    private var loaded = false
    private var pendingStyle: TerminalStylePayload?

    let webView: WKWebView
    private let userContentController: WKUserContentController
    var onInput: ((String) -> Void)?
    var onResize: ((Int, Int) -> Void)?

    override init() {
        let controller = WKUserContentController()
        let config = WKWebViewConfiguration()
        config.userContentController = controller
        self.webView = WKWebView(frame: .zero, configuration: config)
        self.userContentController = controller
        super.init()
        HandlerName.allCases.forEach { controller.add(WeakScriptMessageHandler(handler: self), name: $0.rawValue) }
        webView.isOpaque = false
        webView.backgroundColor = .clear
        webView.scrollView.isScrollEnabled = false
        webView.scrollView.bounces = false
        loadTerminalPage()
    }

    deinit {
        HandlerName.allCases.forEach { userContentController.removeScriptMessageHandler(forName: $0.rawValue) }
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
        outputLock.unlock()
        if shouldFlush {
            DispatchQueue.main.async { [weak self] in
                self?.flushOutput()
            }
        }
    }

    func updateStyle(font: UIFont, foreground: UIColor, background: UIColor) {
        let payload: [String: Any] = [
            "foregroundColor": cssColor(from: foreground),
            "backgroundColor": cssColor(from: background),
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
        if loaded {
            applyStyle(style)
        } else {
            pendingStyle = style
        }
    }

    func userContentController(_ userContentController: WKUserContentController, didReceive message: WKScriptMessage) {
        switch message.name {
        case HandlerName.load.rawValue:
            loaded = true
            if let style = pendingStyle {
                applyStyle(style)
                pendingStyle = nil
            }
            flushOutput()
            requestResize()
        case HandlerName.sendInput.rawValue:
            if let input = message.body as? String {
                onInput?(input)
            }
        case HandlerName.resize.rawValue:
            requestResize()
        case HandlerName.openLink.rawValue:
            if let urlString = message.body as? String, let url = URL(string: urlString) {
                UIApplication.shared.open(url)
            }
        default:
            break
        }
    }

    private func loadTerminalPage() {
        guard let url = terminalPageURL() else { return }
        webView.loadFileURL(url, allowingReadAccessTo: url.deletingLastPathComponent())
    }

    private func terminalPageURL() -> URL? {
        if let url = Bundle.main.url(forResource: "term", withExtension: "html", subdirectory: "TerminalWeb") {
            return url
        }
        return Bundle.main.url(forResource: "term", withExtension: "html")
    }

    private func requestResize() {
        webView.evaluateJavaScript("exports.getSize()") { [weak self] result, _ in
            guard let self = self else { return }
            guard let array = result as? [NSNumber], array.count == 2 else { return }
            self.onResize?(array[0].intValue, array[1].intValue)
        }
    }

    private func flushOutput() {
        guard loaded else {
            outputLock.lock()
            outputInProgress = false
            outputLock.unlock()
            return
        }
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
        webView.evaluateJavaScript(script) { [weak self] _, _ in
            self?.requestResize()
        }
    }

    private func jsWriteString(for data: Data) -> String? {
        guard var string = String(data: data, encoding: .isoLatin1) else { return nil }
        string = string.replacingOccurrences(of: "\\\\", with: "\\\\\\\\")
        string = string.replacingOccurrences(of: "\r", with: "\\r")
        string = string.replacingOccurrences(of: "\n", with: "\\n")
        string = string.replacingOccurrences(of: "\"", with: "\\\"")
        return "exports.write(\"\(string)\")"
    }

    private func cssColor(from color: UIColor) -> String {
        var red: CGFloat = 0
        var green: CGFloat = 0
        var blue: CGFloat = 0
        var alpha: CGFloat = 0
        color.getRed(&red, green: &green, blue: &blue, alpha: &alpha)
        let r = Int(red * 255.0)
        let g = Int(green * 255.0)
        let b = Int(blue * 255.0)
        return String(format: "#%02X%02X%02X", r, g, b)
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

struct HtermTerminalView: UIViewRepresentable {
    let font: UIFont
    let foregroundColor: UIColor
    let backgroundColor: UIColor
    var onInput: ((String) -> Void)?
    var onResize: ((Int, Int) -> Void)?
    var onReady: ((HtermTerminalController) -> Void)?

    func makeCoordinator() -> Coordinator {
        Coordinator()
    }

    func makeUIView(context: Context) -> UIView {
        let container = UIView()
        container.backgroundColor = backgroundColor
        let controller = context.coordinator.controller
        controller.onInput = onInput
        controller.onResize = onResize
        controller.attach(to: container)
        controller.updateStyle(font: font, foreground: foregroundColor, background: backgroundColor)
        onReady?(controller)
        return container
    }

    func updateUIView(_ uiView: UIView, context: Context) {
        let controller = context.coordinator.controller
        controller.onInput = onInput
        controller.onResize = onResize
        controller.updateStyle(font: font, foreground: foregroundColor, background: backgroundColor)
        uiView.backgroundColor = backgroundColor
    }

    final class Coordinator {
        let controller = HtermTerminalController()
    }
}
