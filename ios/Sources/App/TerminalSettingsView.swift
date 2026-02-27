import SwiftUI

struct TerminalSettingsView: View {
    @ObservedObject private var appearanceSettings: TerminalTabAppearanceSettings
    @ObservedObject private var tabManager = TerminalTabManager.shared
    @ObservedObject private var settings = TerminalFontSettings.shared
    @Environment(\.dismiss) private var dismiss
    private let tabId: UInt64
    @State private var tabNameDraft: String
    @State private var resetTabNameOnAppStart: Bool
    @State private var startupCommandDraft: String
    @State private var copiedColors: Bool = false

    init(appearanceSettings: TerminalTabAppearanceSettings, tabId: UInt64) {
        _appearanceSettings = ObservedObject(wrappedValue: appearanceSettings)
        self.tabId = tabId
        _tabNameDraft = State(initialValue: TerminalTabManager.shared.tabTitle(tabId: tabId) ?? "")
        _resetTabNameOnAppStart = State(initialValue: TerminalTabManager.shared.resetTabNameOnAppStart(tabId: tabId))
        _startupCommandDraft = State(initialValue: TerminalTabManager.shared.startupCommand(tabId: tabId) ?? "")
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
                    HStack {
                        Spacer()
                        Text("Sample AaBb123")
                            .font(Font(previewFont))
                            .foregroundColor(Color(appearanceSettings.foregroundColor))
                            .padding(.vertical, 8)
                            .padding(.horizontal, 16)
                            .background(Color(appearanceSettings.backgroundColor))
                            .cornerRadius(8)
                            .overlay(
                                RoundedRectangle(cornerRadius: 8)
                                    .stroke(Color.gray.opacity(0.3), lineWidth: 1)
                            )
                        Spacer()
                    }
                    .padding(.vertical, 4)
                    .accessibilityLabel("Font preview with current settings")
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

                    Toggle(
                        "Restore on app start",
                        isOn: Binding(
                            get: { resetTabNameOnAppStart },
                            set: { enabled in
                                _ = tabManager.updateResetTabNameOnAppStart(tabId: tabId, enabled: enabled)
                                resetTabNameOnAppStart = tabManager.resetTabNameOnAppStart(tabId: tabId)
                            }
                        )
                    )

                    Text("Used for this tab profile. Enable restore to apply the saved name when the app starts.")
                        .font(.footnote)
                        .foregroundColor(.secondary)
                }

                Section(header: Text("Startup Command")) {
                    TextField(
                        "Run on tab startup",
                        text: Binding(
                            get: { startupCommandDraft },
                            set: { newValue in
                                _ = tabManager.updateStartupCommand(tabId: tabId, rawCommand: newValue)
                                startupCommandDraft = tabManager.startupCommand(tabId: tabId) ?? newValue
                            }
                        )
                    )
                    .textInputAutocapitalization(.never)
                    .autocorrectionDisabled(true)

                    Text("Runs once each time this tab profile launches. Leave empty to disable.")
                        .font(.footnote)
                        .foregroundColor(.secondary)
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

                    Button {
                        _ = tabManager.copyFirstTabColors(tabId: tabId)
                        copiedColors = true
                        DispatchQueue.main.asyncAfter(deadline: .now() + 2) {
                            copiedColors = false
                        }
                    } label: {
                        HStack {
                            Text(copiedColors ? "Copied!" : "Copy First Tab Values")
                            if copiedColors {
                                Image(systemName: "checkmark.circle.fill")
                                    .foregroundColor(.green)
                            }
                        }
                    }
                }

                Section(header: Text("Filesystem Paths")) {
                    Toggle(
                        "Present sandbox as /",
                        isOn: Binding(
                            get: { settings.pathTruncationEnabled },
                            set: { settings.updatePathTruncationEnabled($0) }
                        )
                    )

                    Text("Requires restarting PSCAL to take effect.")
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
                        Text("Four").tag(4)
                        Text("Five").tag(5)
                    }
                    .pickerStyle(.segmented)

                    Text("Opens the chosen number of shell tabs when PSCAL starts.")
                        .font(.footnote)
                        .foregroundColor(.secondary)
                }

                Section(header: Text("Init (Service Mode)")) {
                    Toggle(
                        "Run init in first tab",
                        isOn: Binding(
                            get: { settings.runInitAsPid1OnFirstTab },
                            set: { settings.updateRunInitAsPid1OnFirstTab($0) }
                        )
                    )

                    Text("When enabled, PSCAL starts tab 1 with init --service-mode as the first process.")
                        .font(.footnote)
                        .foregroundColor(.secondary)

                    Text("Default is off. Restart PSCAL to apply.")
                        .font(.footnote)
                        .foregroundColor(.secondary)

                    Text("Init is non-interactive; open another tab for a shell prompt.")
                        .font(.footnote)
                        .foregroundColor(.secondary)
                }

                Section {
                    VStack(spacing: 10) {
                        if let url = URL(string: "https://discord.gg/YWQVExN363") {
                            Link(destination: url) {
                                HStack {
                                    Text("Join the PSCAL Discord")
                                    Image(systemName: "arrow.up.right.square")
                                }
                            }
                            .accessibilityLabel("Join the P S C A L Discord server")
                        }

                        if let github = URL(string: "https://github.com/emkey1/smallclue") {
                            Link(destination: github) {
                                HStack {
                                    Text("Report bugs on GitHub")
                                    Image(systemName: "arrow.up.right.square")
                                }
                            }
                            .accessibilityLabel("Report bugs and suggestions on GitHub")
                        }
                    }
                    .font(.callout)
                    .frame(maxWidth: .infinity)
                    .foregroundColor(.blue)
                }
            }
            .navigationTitle("Terminal Settings")
            .toolbar {
                ToolbarItem(placement: .confirmationAction) {
                    Button("Done") { dismiss() }
                }
            }
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
