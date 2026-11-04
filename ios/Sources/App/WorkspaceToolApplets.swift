// Copyright 2026 Mike Miller
// SPDX-License-Identifier: Apache-2.0
//
// Ported by its author from iSH-AOK's Workspace (app/WorkspaceViewController.m at
// 0f75fee1c) and licensed to PSCAL under the Apache License 2.0.

import SwiftUI
import UIKit

/// The most used tools and a new terminal, as big buttons.
@MainActor
final class WorkspaceQuickActionsAppletViewController: WorkspaceAppletViewController, WorkspaceTextScalable {
    private static let newTerminal = "shell"

    private var contentStack: UIStackView!
    private var headerLabel: UILabel!
    private var detailLabel: UILabel!
    private var buttons: [WorkspaceTwoLineButton] = []
    private var minimumHeights: [NSLayoutConstraint] = []
    private var identifierByButton: [ObjectIdentifier: String] = [:]

    private var buttonMinimumHeight: CGFloat {
        ((WorkspaceMetrics.usesPhoneLayout ? 52 : 58) * workspaceTextScale).rounded(.up)
    }

    override func viewDidLoad() {
        super.viewDidLoad()
        contentStack = installScrollingStack(spacing: 6, inset: 8).stack
        headerLabel = themeAccentLabel(style: .headline)
        headerLabel.text = "Quick workspace actions"
        detailLabel = themeSecondaryLabel(style: .footnote)
        detailLabel.text = "Open the most common tools and a new terminal without leaving the workspace."
        contentStack.addArrangedSubview(headerLabel)
        contentStack.addArrangedSubview(detailLabel)

        let actions: [(title: String, subtitle: String, identifier: String)] = [
            ("Desktops", "Manage in-app Desktops", WorkspaceToolRegistry.desktops),
            ("New Terminal", "Open a new shell window", Self.newTerminal),
            ("Sessions", "Inspect live terminals", WorkspaceToolRegistry.sessions),
            ("Storage", "App and folder usage", WorkspaceToolRegistry.storage),
            ("Themes", "Colors, density, wallpaper", WorkspaceToolRegistry.themes),
            ("Clock", "Quick local time", WorkspaceToolRegistry.clock),
        ].filter { $0.identifier == Self.newTerminal || WorkspaceToolRegistry.isAvailable($0.identifier) }

        var index = 0
        while index < actions.count {
            let row = UIStackView()
            row.axis = .horizontal
            row.spacing = 6
            row.distribution = .fillEqually
            for action in actions[index..<min(index + 2, actions.count)] {
                let button = twoLineButton(title: action.title, subtitle: action.subtitle, action: #selector(runAction(_:)))
                button.accessibilityIdentifier = "workspace.quick.\(action.identifier)"
                button.contentEdgeInsets = UIEdgeInsets(top: 8, left: 10, bottom: 8, right: 10)
                button.setContentCompressionResistancePriority(.required, for: .vertical)
                let minimum = button.heightAnchor.constraint(greaterThanOrEqualToConstant: buttonMinimumHeight)
                minimum.isActive = true
                minimumHeights.append(minimum)
                identifierByButton[ObjectIdentifier(button)] = action.identifier
                buttons.append(button)
                row.addArrangedSubview(button)
            }
            contentStack.addArrangedSubview(row)
            index += 2
        }
    }

    override func viewDidLayoutSubviews() {
        super.viewDidLayoutSubviews()
        contentStack.spacing = WorkspaceTheme.densityValue(4, 6)
    }

    override func applyTextScale() {
        super.applyTextScale()
        guard contentStack != nil else { return }
        headerLabel.font = .systemFont(ofSize: toolScaledFontSize(WorkspaceTheme.fontSize(.headline)))
        detailLabel.font = .systemFont(ofSize: toolScaledFontSize(WorkspaceTheme.fontSize(.footnote)))
        minimumHeights.forEach { $0.constant = buttonMinimumHeight }
    }

    @objc private func runAction(_ sender: WorkspaceTwoLineButton) {
        guard let identifier = identifierByButton[ObjectIdentifier(sender)] else { return }
        if identifier == Self.newTerminal {
            workspaceHost?.openNewTerminalWindow()
        } else {
            workspaceHost?.openOrFocusTool(identifier)
        }
    }
}

/// How many sessions are running, and a row for each that goes to its window.
@MainActor
final class WorkspaceSessionsAppletViewController: WorkspaceAppletViewController, WorkspaceTextScalable {
    private var contentStack: UIStackView!
    private var summaryLabel: UILabel!
    private var eyebrowLabel: UILabel!
    private var sessionsTitleLabel: UILabel!
    private var sessionRows: UIStackView!
    private var tabIDByButton: [ObjectIdentifier: UInt64] = [:]
    private var observer: NSObjectProtocol?

    deinit {
        if let observer {
            NotificationCenter.default.removeObserver(observer)
        }
    }

    override func viewDidLoad() {
        super.viewDidLoad()
        contentStack = installScrollingStack(spacing: 8, inset: 6).stack

        let summary = stackCard(spacing: 6, inset: 12)
        eyebrowLabel = themeSecondaryLabel(style: .caption1)
        eyebrowLabel.text = "ACTIVE SESSIONS"
        eyebrowLabel.font = .systemFont(ofSize: 9, weight: .semibold)
        summaryLabel = themeAccentLabel(style: .headline)
        summary.stack.addArrangedSubview(eyebrowLabel)
        summary.stack.addArrangedSubview(summaryLabel)

        let quick = stackCard(spacing: 6, inset: 8)
        let row = UIStackView()
        row.axis = .horizontal
        row.spacing = 6
        row.distribution = .fillEqually
        row.addArrangedSubview(twoLineButton(title: "New Terminal", subtitle: "Open a new shell window", action: #selector(openTerminal)))
        row.addArrangedSubview(twoLineButton(title: "Desktops", subtitle: "Manage in-app Desktops", action: #selector(openDesktops)))
        quick.stack.addArrangedSubview(row)

        let sessions = stackCard(spacing: 6, inset: 10)
        sessionsTitleLabel = themePrimaryLabel(style: .subheadline)
        sessionsTitleLabel.text = "Live terminals"
        sessionRows = UIStackView()
        sessionRows.axis = .vertical
        sessionRows.spacing = 6
        sessions.stack.addArrangedSubview(sessionsTitleLabel)
        sessions.stack.addArrangedSubview(sessionRows)

        contentStack.addArrangedSubview(summary.card)
        contentStack.addArrangedSubview(quick.card)
        contentStack.addArrangedSubview(sessions.card)

        // Workspace says so whenever its sessions, windows or Desktops change.
        observer = NotificationCenter.default.addObserver(forName: .workspaceDesktopsDidChange, object: nil, queue: .main) { [weak self] _ in
            MainActor.assumeIsolated {
                self?.refreshSessions()
            }
        }
        refreshSessions()
    }

    override func viewWillAppear(_ animated: Bool) {
        super.viewWillAppear(animated)
        refreshSessions()
    }

    override func viewDidLayoutSubviews() {
        super.viewDidLayoutSubviews()
        contentStack.spacing = WorkspaceTheme.densityValue(4, 8)
        sessionRows.spacing = WorkspaceTheme.densityValue(4, 6)
    }

    private func refreshSessions() {
        guard let host = workspaceHost else { return }
        let manager = TerminalTabManager.shared
        let tabs = manager.tabs.filter { tab in
            if case .sdl = tab.kind { return false }
            return true
        }
        let windowCount = host.desktopWindows.filter { $0.toolIdentifier == nil }.count
        summaryLabel.text = "\(tabs.count) terminals  •  \(windowCount) windows  •  \(host.desktopCount) Desktops"

        clearArrangedSubviews(of: sessionRows)
        tabIDByButton.removeAll()
        if tabs.isEmpty {
            let empty = themeSecondaryLabel(style: .footnote)
            empty.font = .systemFont(ofSize: toolScaledFontSize(WorkspaceTheme.fontSize(.footnote)))
            empty.text = "No active terminals. Use New Terminal above to start a shell."
            sessionRows.addArrangedSubview(empty)
            return
        }
        for tab in tabs {
            let name = manager.tabTitle(tabId: tab.id) ?? tab.title
            let subtitle: String
            if let window = host.windowHosting(tabID: tab.id) {
                subtitle = window.desktopIndex == host.activeDesktopIndex
                    ? "Go to its window"
                    : "Go to its window on \(host.nameForDesktop(window.desktopIndex))"
            } else {
                subtitle = "Open it in a window"
            }
            let button = twoLineButton(title: name, subtitle: subtitle, action: #selector(openSession(_:)))
            button.accessibilityIdentifier = "workspace.sessions.\(tab.id)"
            tabIDByButton[ObjectIdentifier(button)] = tab.id
            sessionRows.addArrangedSubview(button)
        }
    }

    override func applyTheme() {
        super.applyTheme()
        summaryLabel?.textColor = theme.accentAlt
    }

    override func applyTextScale() {
        super.applyTextScale()
        guard contentStack != nil else { return }
        eyebrowLabel.font = .systemFont(ofSize: toolScaledFontSize(9), weight: .semibold)
        summaryLabel.font = .systemFont(ofSize: toolScaledFontSize(WorkspaceTheme.fontSize(.headline)))
        sessionsTitleLabel.font = .systemFont(ofSize: toolScaledFontSize(WorkspaceTheme.fontSize(.subheadline)))
        refreshSessions()
    }

    @objc private func openTerminal() {
        workspaceHost?.openNewTerminalWindow()
    }

    @objc private func openDesktops() {
        workspaceHost?.openOrFocusTool(WorkspaceToolRegistry.desktops)
    }

    @objc private func openSession(_ sender: WorkspaceTwoLineButton) {
        guard let tabID = tabIDByButton[ObjectIdentifier(sender)] else { return }
        workspaceHost?.focusSession(tabID: tabID)
    }
}

/// A SwiftUI screen filling an applet's window.
@MainActor
class WorkspaceHostedAppletViewController: WorkspaceAppletViewController {
    func embed<Content: View>(_ rootView: Content) {
        let hosting = UIHostingController(rootView: rootView)
        addChild(hosting)
        hosting.view.translatesAutoresizingMaskIntoConstraints = false
        toolContentView.addSubview(hosting.view)
        NSLayoutConstraint.activate([
            hosting.view.topAnchor.constraint(equalTo: toolContentView.topAnchor),
            hosting.view.leadingAnchor.constraint(equalTo: toolContentView.leadingAnchor),
            hosting.view.trailingAnchor.constraint(equalTo: toolContentView.trailingAnchor),
            hosting.view.bottomAnchor.constraint(equalTo: toolContentView.bottomAnchor),
        ])
        hosting.didMove(toParent: self)
    }
}

private struct WorkspaceSettingsPage: View {
    @State private var opensAtLaunch = WorkspaceSettings.opensAtLaunch
    let openThemes: () -> Void
    let terminalSettings: () -> AnyView?

    var body: some View {
        NavigationView {
            Form {
                Section(header: Text("Workspace")) {
                    Toggle("Open at Launch", isOn: $opensAtLaunch)
                        .onChange(of: opensAtLaunch) { value in
                            WorkspaceSettings.opensAtLaunch = value
                        }
                    Button("Themes, Density and Gauges", action: openThemes)
                }
                Section(header: Text("Terminal")) {
                    if let settings = terminalSettings() {
                        NavigationLink("Settings for the Selected Terminal") {
                            settings
                        }
                    } else {
                        Text("Select a terminal window first.")
                            .foregroundColor(.secondary)
                    }
                }
            }
            .navigationTitle("Settings")
            .navigationBarTitleDisplayMode(.inline)
        }
        .navigationViewStyle(.stack)
    }
}

/// The app's terminal settings, with Workspace's own beside them.
@MainActor
final class WorkspaceSettingsAppletViewController: WorkspaceHostedAppletViewController {
    override func viewDidLoad() {
        super.viewDidLoad()
        embed(WorkspaceSettingsPage(
            openThemes: { [weak self] in
                self?.workspaceHost?.openOrFocusTool(WorkspaceToolRegistry.themes)
            },
            terminalSettings: {
                let manager = TerminalTabManager.shared
                guard let tab = manager.tabs.first(where: { $0.id == manager.selectedId }) else { return nil }
                return AnyView(TerminalSettingsView(appearanceSettings: tab.appearanceSettings,
                                                    tabId: tab.id,
                                                    showsDoneButton: false))
            }))
    }
}

/// The app's diagnostics in a window.
@MainActor
final class WorkspaceDiagnosticsAppletViewController: WorkspaceHostedAppletViewController {
    override func viewDidLoad() {
        super.viewDidLoad()
        embed(AppDiagnosticsView(showsDoneButton: false))
    }
}
