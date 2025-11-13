import SwiftUI
import UIKit
import Combine

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
        VStack(spacing: 0) {
            ScrollViewReader { proxy in
                ScrollView {
                    LazyVStack(alignment: .leading, spacing: 0) {
                        ForEach(Array(runtime.screenLines.enumerated()), id: \.offset) { index, line in
                            AttributedTextLine(line: line)
                                .frame(maxWidth: .infinity, alignment: .leading)
                                .id(index)
                        }
                    }
                }
                .background(Color(.systemBackground))
                .onChange(of: runtime.screenLines.count) { count in
                    withAnimation {
                        proxy.scrollTo(max(count - 1, 0), anchor: .bottom)
                    }
                }
            }

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
        .background(Color(.systemBackground))
        .contentShape(Rectangle())
        .onTapGesture {
            focusAnchor += 1
        }
        .overlay(alignment: .bottomLeading) {
            TerminalInputBridge(focusAnchor: $focusAnchor, onInput: handleInput)
                .frame(width: 0, height: 0)
                .allowsHitTesting(false)
        }
        .onAppear {
            updateTerminalGeometry()
            runtime.start()
            focusAnchor += 1
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

private struct AttributedTextLine: UIViewRepresentable {
    let line: NSAttributedString

    func makeUIView(context: Context) -> UILabel {
        let label = UILabel()
        label.numberOfLines = 0
        label.backgroundColor = .clear
        label.setContentCompressionResistancePriority(.defaultLow, for: .horizontal)
        return label
    }

    func updateUIView(_ uiView: UILabel, context: Context) {
        uiView.attributedText = line
    }
}
