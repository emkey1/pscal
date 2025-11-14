import SwiftUI
import UIKit
import Combine
import QuartzCore

struct TerminalView: View {
    var body: some View {
        KeyboardAwareContainer(
            content: GeometryReader { proxy in
                TerminalContentView(availableSize: proxy.size)
                    .frame(width: proxy.size.width, height: proxy.size.height)
            }
        )
        .edgesIgnoringSafeArea(.bottom)
    }
}

private struct TerminalContentView: View {
    let availableSize: CGSize
    @ObservedObject private var runtime = PscalRuntimeBootstrap.shared
    @State private var focusAnchor: Int = 0

    var body: some View {
        let background = runtime.terminalBackgroundColor
        return VStack(spacing: 0) {
            TerminalTextView(text: runtime.screenText,
                             cursor: runtime.cursorInfo,
                             backgroundColor: background)
                .frame(maxWidth: .infinity, maxHeight: .infinity)
                .background(Color(background))

            Divider()

            if let status = runtime.exitStatus {
                Text("Process exited with status \(status)")
                    .font(.footnote)
                    .foregroundColor(.secondary)
                    .padding(.vertical, 8)
                    .frame(maxWidth: .infinity)
                    .background(Color(.secondarySystemBackground))
            }
        }
        .background(Color(background))
        .contentShape(Rectangle())
        .onTapGesture {
            focusAnchor &+= 1
        }
        .overlay(alignment: .bottomLeading) {
            TerminalInputBridge(focusAnchor: $focusAnchor, onInput: handleInput)
                .frame(width: 1, height: 1)
                .allowsHitTesting(false)
        }
        .onAppear {
            updateTerminalGeometry()
            runtime.start()
            focusAnchor &+= 1
        }
        .onChange(of: availableSize) { _ in
            updateTerminalGeometry()
        }
        .onChange(of: runtime.exitStatus) { _ in
            updateTerminalGeometry()
        }
        .onReceive(NotificationCenter.default.publisher(for: UIContentSizeCategory.didChangeNotification)) { _ in
            updateTerminalGeometry()
        }
    }

    private func handleInput(_ text: String) {
        runtime.send(text)
    }

    private func updateTerminalGeometry() {
        guard let metrics = TerminalGeometryCalculator.metrics(for: availableSize,
                                                               showingStatus: runtime.exitStatus != nil) else {
            return
        }
        runtime.updateTerminalSize(columns: metrics.columns, rows: metrics.rows)
    }
}

struct TerminalView_Previews: PreviewProvider {
    static var previews: some View {
        TerminalView()
            .previewDevice("iPad Pro (11-inch) (4th generation)")
    }
}

private struct TerminalGeometryMetrics: Equatable {
    let columns: Int
    let rows: Int
}

private enum TerminalGeometryCalculator {
    private static let horizontalPadding: CGFloat = 16.0 // padding(.horizontal, 8) * 2
    private static let verticalRowPadding: CGFloat = 4.0  // padding(.vertical, 2) * 2
    private static let statusOverlayHeight: CGFloat = 32.0

    static func metrics(for size: CGSize, showingStatus: Bool) -> TerminalGeometryMetrics? {
        guard size.width > 0, size.height > 0 else { return nil }

        let font = UIFont.monospacedSystemFont(ofSize: UIFont.preferredFont(forTextStyle: .body).pointSize, weight: .regular)
        let charWidth = max(1.0, ("W" as NSString).size(withAttributes: [.font: font]).width)
        let lineHeight = font.lineHeight + verticalRowPadding

        let usableWidth = max(0, size.width - horizontalPadding)
        var usableHeight = size.height
        if showingStatus {
            usableHeight -= statusOverlayHeight
        }

        let rawColumns = Int(floor(usableWidth / charWidth))
        let rawRows = Int(floor(usableHeight / lineHeight))
        guard rawColumns > 0, rawRows > 0 else { return nil }

        return TerminalGeometryMetrics(
            columns: max(10, rawColumns),
            rows: max(4, rawRows)
        )
    }
}

private struct TerminalTextView: UIViewRepresentable {
    let text: NSAttributedString
    let cursor: TerminalCursorInfo?
    let backgroundColor: UIColor

    func makeUIView(context: Context) -> TerminalDisplayTextView {
        let textView = TerminalDisplayTextView()
        textView.isEditable = false
        textView.isSelectable = true
        textView.isScrollEnabled = true
        textView.backgroundColor = backgroundColor
        textView.textContainerInset = .zero
        textView.textContainer.lineFragmentPadding = 0
        textView.alwaysBounceVertical = true
        textView.setContentCompressionResistancePriority(.defaultLow, for: .horizontal)
        textView.font = TerminalFontMetrics.displayFont
        return textView
    }

    func updateUIView(_ uiView: TerminalDisplayTextView, context: Context) {
        uiView.attributedText = text
        uiView.cursorInfo = cursor
        uiView.backgroundColor = backgroundColor
        let length = text.length
        guard length > 0 else { return }
        let bottomRange = NSRange(location: length - 1, length: 1)
        DispatchQueue.main.async {
            let currentLength = uiView.attributedText.length
            guard bottomRange.location < currentLength else { return }
            uiView.scrollRangeToVisible(bottomRange)
        }
    }
}

private enum TerminalFontMetrics {
    static var displayFont: UIFont {
        UIFont.monospacedSystemFont(ofSize: UIFont.preferredFont(forTextStyle: .body).pointSize, weight: .regular)
    }

    static var characterWidth: CGFloat {
        let font = displayFont
        return max(1, ("W" as NSString).size(withAttributes: [.font: font]).width)
    }

    static var lineHeight: CGFloat {
        displayFont.lineHeight
    }
}

final class TerminalDisplayTextView: UITextView {
    var cursorInfo: TerminalCursorInfo? {
        didSet { updateCursorLayer() }
    }

    private let cursorLayer: CALayer = {
        let layer = CALayer()
        layer.backgroundColor = UIColor.systemOrange.cgColor
        layer.opacity = 0
        return layer
    }()

    private var blinkAnimationAdded = false

    override init(frame: CGRect, textContainer: NSTextContainer?) {
        super.init(frame: frame, textContainer: textContainer)
        layer.addSublayer(cursorLayer)
        isEditable = false
        isScrollEnabled = true
        textContainerInset = .zero
        backgroundColor = .clear
    }

    required init?(coder: NSCoder) {
        fatalError("init(coder:) has not been implemented")
    }

    override func layoutSubviews() {
        super.layoutSubviews()
        updateCursorLayer()
    }

    override var contentOffset: CGPoint {
        didSet { updateCursorLayer() }
    }

    private func updateCursorLayer() {
        guard let info = cursorInfo else {
            cursorLayer.opacity = 0
            return
        }

        let charWidth = TerminalFontMetrics.characterWidth
        let lineHeight = TerminalFontMetrics.lineHeight
        let x = CGFloat(info.column) * charWidth - contentOffset.x + textContainerInset.left
        let y = CGFloat(info.row) * lineHeight - contentOffset.y + textContainerInset.top
        cursorLayer.frame = CGRect(x: x, y: y, width: max(1, charWidth), height: lineHeight)

        if !blinkAnimationAdded {
            let animation = CABasicAnimation(keyPath: "opacity")
            animation.fromValue = 1
            animation.toValue = 0
            animation.duration = 0.8
            animation.autoreverses = true
            animation.repeatCount = .infinity
            cursorLayer.add(animation, forKey: "blink")
            blinkAnimationAdded = true
        }

        cursorLayer.opacity = 1
    }
}
