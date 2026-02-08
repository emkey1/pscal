import SwiftUI

struct TerminalSettingsView: View {
    @ObservedObject private var appearanceSettings: TerminalTabAppearanceSettings
    @ObservedObject private var tabManager = TerminalTabManager.shared
    @ObservedObject private var settings = TerminalFontSettings.shared
    @Environment(\.dismiss) private var dismiss
    private let tabId: UInt64
    @State private var tabNameDraft: String

    init(appearanceSettings: TerminalTabAppearanceSettings, tabId: UInt64) {
        _appearanceSettings = ObservedObject(wrappedValue: appearanceSettings)
        self.tabId = tabId
        _tabNameDraft = State(initialValue: TerminalTabManager.shared.tabTitle(tabId: tabId) ?? "")
    }

    var body: some View {
        content
    }

    @ViewBuilder
    private var content: some View {
        NavigationView {
            Form {
                Section(header: Text("Font")) {
                    Picker("Font",
                           selection: Binding(
                            get: { appearanceSettings.selectedFontID },
                            set: { appearanceSettings.updateFontSelection($0) }
                           )) {
                        ForEach(appearanceSettings.fontOptions) { option in
                            Text(option.displayName).tag(option.id)
                        }
                    }

                    let previewFont = appearanceSettings.font(forPointSize: 16)
                    Text("Sample AaBb123")
                        .font(Font(previewFont))
                        .foregroundColor(Color(appearanceSettings.foregroundColor))
                }

                Section(header: Text("Tab Name")) {
                    TextField(
                        "Tab name",
                        text: Binding(
                            get: { tabNameDraft },
                            set: { newValue in
                                _ = tabManager.updateTabTitle(tabId: tabId, rawTitle: newValue)
                                tabNameDraft = tabManager.tabTitle(tabId: tabId) ?? newValue
                            }
                        )
                    )
                    .textInputAutocapitalization(.never)
                    .autocorrectionDisabled(true)

                    Text("Used for this tab profile and restored on next launch.")
                        .font(.footnote)
                        .foregroundColor(.secondary)
                }

                Section(header: Text("Font Size")) {
                    Slider(
                        value: Binding(
                            get: { Double(appearanceSettings.pointSize) },
                            set: { appearanceSettings.updatePointSize(CGFloat($0)) }
                        ),
                        in: Double(appearanceSettings.minimumPointSize)...Double(appearanceSettings.maximumPointSize),
                        step: 1
                    )

                    HStack {
                        Text("Current")
                        Spacer()
                        Text("\(Int(appearanceSettings.pointSize)) pt")
                            .font(.system(.body, design: .monospaced))
                    }
                }

                Section(header: Text("Colors")) {
                    ColorPicker(
                        "Background",
                        selection: Binding(
                            get: { Color(appearanceSettings.backgroundColor) },
                            set: { appearanceSettings.updateBackgroundColor(UIColor(swiftUIColor: $0)) }
                        )
                    )

                    ColorPicker(
                        "Foreground",
                        selection: Binding(
                            get: { Color(appearanceSettings.foregroundColor) },
                            set: { appearanceSettings.updateForegroundColor(UIColor(swiftUIColor: $0)) }
                        )
                    )
                }

                Section(header: Text("Filesystem Paths")) {
                    Toggle(
                        "Present sandbox as /",
                        isOn: Binding(
                            get: { settings.pathTruncationEnabled },
                            set: { settings.updatePathTruncationEnabled($0) }
                        )
                    )

                    Text("Requires restarting PSCAL take effect.")
                        .font(.footnote)
                        .foregroundColor(.secondary)

                    TextField(
                        "PATH_TRUNCATE",
                        text: Binding(
                            get: { settings.pathTruncationPath },
                            set: { settings.updatePathTruncationPath($0) }
                        )
                    )
                    .textInputAutocapitalization(.none)
                    .autocorrectionDisabled(true)
                    .font(.system(.body, design: .monospaced))
                    .disabled(!settings.pathTruncationEnabled)

                    Button("Use Documents root") {
                        settings.useDocumentsPathForTruncation()
                    }
                    .disabled(!settings.pathTruncationEnabled)

                    Text(
                        settings.pathTruncationEnabled
                        ? "Absolute paths map to \(settings.pathTruncationPath.isEmpty ? "Documents" : settings.pathTruncationPath)."
                        : "PATH_TRUNCATE is disabled; absolute paths reflect the full sandbox path."
                    )
                    .font(.footnote)
                    .foregroundColor(.secondary)

                    Text("Files app: PSCAL shows up under On My iPad; it exposes Documents/ (including home/).")
                        .font(.footnote)
                        .foregroundColor(.secondary)
                }

                Section(header: Text("Devices")) {
                    Toggle(
                        "Enable /dev/location",
                        isOn: Binding(
                            get: { settings.locationDeviceEnabled },
                            set: { settings.updateLocationDeviceEnabled($0) }
                        )
                    )

                    Text("Expose a live location feed to shell sessions.")
                        .font(.footnote)
                        .foregroundColor(.secondary)
                }

                Section(header: Text("Tabs at App Launch")) {
                    Picker("Tabs on launch", selection: Binding(
                        get: { settings.initialTabCount },
                        set: { settings.updateInitialTabCount($0) }
                    )) {
                        Text("One").tag(1)
                        Text("Two").tag(2)
                        Text("Three").tag(3)
                    }
                    .pickerStyle(.segmented)

                    Text("Opens the chosen number of shell tabs when PSCAL starts.")
                        .font(.footnote)
                        .foregroundColor(.secondary)
                }
            }
            .navigationTitle("Terminal Settings")
            .toolbar {
                ToolbarItem(placement: .confirmationAction) {
                    Button("Done") { dismiss() }
                }
            }
        }
        .safeAreaInset(edge: .bottom) {
            VStack(spacing: 6) {
                Divider()
                VStack(spacing: 6) {
                    HStack(spacing: 6) {
                        Text("Subscribe to the PSCAL")
                        if let url = URL(string: "https://discord.gg/YWQVExN363") {
                            Link(destination: url) {
                                HStack(spacing: 4) {
                                    Text("Discord").underline()
                                    Image(systemName: "arrow.up.right.square")
                                }
                                .foregroundColor(.blue)
                            }
                        }
                    }
                    .font(.callout.weight(.semibold))
                    .multilineTextAlignment(.center)

                    if let github = URL(string: "https://github.com/emkey1/smallclue") {
                        HStack(spacing: 4) {
                            Text("Please submit bugs and suggestions on")
                            Link(destination: github) {
                                HStack(spacing: 4) {
                                    Text("GitHub").underline()
                                    Image(systemName: "arrow.up.right.square")
                                }
                                .foregroundColor(.blue)
                            }
                        }
                        .font(.callout)
                        .multilineTextAlignment(.center)
                    }
                }
                .foregroundColor(.primary)
            }
            .padding(.vertical, 10)
            .padding(.horizontal)
            .background(.ultraThinMaterial)
        }
        .applyDetents()
    }
}

// MARK: - Sheet Detents Helpers

@available(iOS 16.0, *)
private struct DetentModifier: ViewModifier {
    func body(content: Content) -> some View {
        content.presentationDetents([.medium, .large])
    }
}

private struct LegacyDetentModifier: ViewModifier {
    func body(content: Content) -> some View {
        content
    }
}

private extension View {
    func applyDetents() -> some View {
        if #available(iOS 16.0, *) {
            return AnyView(self.modifier(DetentModifier()))
        } else {
            return AnyView(self.modifier(LegacyDetentModifier()))
        }
    }
}

extension UIColor {
    convenience init(swiftUIColor: Color) {
        if let cgColor = swiftUIColor.cgColor {
            self.init(cgColor: cgColor)
        } else {
            self.init(red: 1, green: 1, blue: 1, alpha: 1)
        }
    }
}
