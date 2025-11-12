import SwiftUI
import UIKit

struct TerminalView: View {
    var body: some View {
        KeyboardAwareContainer(content: TerminalContentView())
            .edgesIgnoringSafeArea(.bottom)
    }
}

private struct TerminalContentView: View {
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
                                .padding(.horizontal, 8)
                                .padding(.vertical, 2)
                                .background(Color(.systemBackground))
                                .id(index)
                        }
                    }
                }
                .background(Color(.secondarySystemBackground))
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
            runtime.start()
            focusAnchor += 1
        }
    }

    private func handleInput(_ text: String) {
        runtime.send(text)
    }
}

struct TerminalView_Previews: PreviewProvider {
    static var previews: some View {
        TerminalView()
            .previewDevice("iPad Pro (11-inch) (4th generation)")
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
