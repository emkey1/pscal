// Copyright 2026 Mike Miller
// SPDX-License-Identifier: Apache-2.0
//
// Ported by its author from iSH-AOK's Workspace (app/WorkspaceViewController.m at
// 0f75fee1c) and licensed to PSCAL under the Apache License 2.0.

import SwiftUI
import UIKit

/// What a session's view needs to know about where it is shown.
@MainActor
final class WorkspaceTerminalHostState: ObservableObject {
    @Published var isVisible = true
    @Published var isFocused = false
}

/// One session's terminal inside a Workspace window.
struct WorkspaceTerminalHostView: View {
    private static let topPadding: CGFloat = 4

    @ObservedObject var state: WorkspaceTerminalHostState
    let tab: TerminalTabManager.Tab

    var body: some View {
        switch tab.kind {
        case .shell(let runtime):
            TerminalView(showsOverlay: false,
                         isVisible: state.isVisible,
                         isActive: state.isFocused,
                         topPadding: Self.topPadding,
                         runtime: runtime,
                         tabId: tab.id,
                         appearanceSettings: tab.appearanceSettings)
        case .shellSession(let session), .ssh(let session):
            ShellTerminalView(session: session,
                              isVisible: state.isVisible,
                              isActive: state.isFocused,
                              topPadding: Self.topPadding,
                              showsOverlay: false,
                              tabId: tab.id,
                              appearanceSettings: tab.appearanceSettings)
        case .sdl:
            SdlTabView {
                TerminalTabManager.shared.selectFirstNonSdlTab()
            } onPromote: {
                TerminalTabManager.shared.promoteSdlToFront()
            }
        }
    }
}

/// A Workspace window holds its sessions as tabs. The strip of numbered tabs appears once
/// there are two.
@MainActor
final class WorkspaceTerminalTabsViewController: UIViewController {
    private static let stripHeight: CGFloat = 30

    final class HostedTab {
        let tabID: UInt64
        let state: WorkspaceTerminalHostState
        let hosting: UIHostingController<WorkspaceTerminalHostView>

        init(tabID: UInt64, state: WorkspaceTerminalHostState, hosting: UIHostingController<WorkspaceTerminalHostView>) {
            self.tabID = tabID
            self.state = state
            self.hosting = hosting
        }
    }

    weak var windowView: WorkspaceWindowView?
    var newTabHandler: (() -> Void)?
    var closeTabHandler: ((UInt64) -> Void)?
    var selectionHandler: ((UInt64) -> Void)?

    private(set) var hostedTabs: [HostedTab] = []
    private(set) var selectedIndex = 0
    private var windowHasFocus = false
    private var windowIsVisible = true

    private let strip = UIStackView()
    private let content = UIView()
    private var stripHeightConstraint: NSLayoutConstraint?
    private var contentBottomConstraint: NSLayoutConstraint?

    var selectedTabID: UInt64? {
        hostedTabs.indices.contains(selectedIndex) ? hostedTabs[selectedIndex].tabID : nil
    }

    override func viewDidLoad() {
        super.viewDidLoad()
        strip.axis = .horizontal
        strip.spacing = 6
        strip.alignment = .center
        strip.translatesAutoresizingMaskIntoConstraints = false
        strip.layoutMargins = UIEdgeInsets(top: 0, left: 8, bottom: 0, right: 8)
        strip.isLayoutMarginsRelativeArrangement = true
        strip.clipsToBounds = true
        content.translatesAutoresizingMaskIntoConstraints = false
        view.addSubview(strip)
        view.addSubview(content)
        let height = strip.heightAnchor.constraint(equalToConstant: 0)
        let bottom = content.bottomAnchor.constraint(equalTo: view.bottomAnchor)
        stripHeightConstraint = height
        contentBottomConstraint = bottom
        NSLayoutConstraint.activate([
            strip.topAnchor.constraint(equalTo: view.topAnchor),
            strip.leadingAnchor.constraint(equalTo: view.leadingAnchor),
            strip.trailingAnchor.constraint(equalTo: view.trailingAnchor),
            height,
            content.topAnchor.constraint(equalTo: strip.bottomAnchor),
            content.leadingAnchor.constraint(equalTo: view.leadingAnchor),
            content.trailingAnchor.constraint(equalTo: view.trailingAnchor),
            bottom,
        ])
    }

    func contains(tabID: UInt64) -> Bool {
        hostedTabs.contains { $0.tabID == tabID }
    }

    func addTab(_ tab: TerminalTabManager.Tab, select: Bool) {
        loadViewIfNeeded()
        let state = WorkspaceTerminalHostState()
        let hosting = UIHostingController(rootView: WorkspaceTerminalHostView(state: state, tab: tab))
        hosting.view.backgroundColor = .clear
        addChild(hosting)
        hosting.view.translatesAutoresizingMaskIntoConstraints = false
        content.addSubview(hosting.view)
        NSLayoutConstraint.activate([
            hosting.view.topAnchor.constraint(equalTo: content.topAnchor),
            hosting.view.leadingAnchor.constraint(equalTo: content.leadingAnchor),
            hosting.view.trailingAnchor.constraint(equalTo: content.trailingAnchor),
            hosting.view.bottomAnchor.constraint(equalTo: content.bottomAnchor),
        ])
        hosting.didMove(toParent: self)
        hostedTabs.append(HostedTab(tabID: tab.id, state: state, hosting: hosting))
        if select || hostedTabs.count == 1 {
            selectedIndex = hostedTabs.count - 1
        }
        applyTabStates()
        refreshStrip()
    }

    /// Takes a tab out when its session has gone. Returns whether it was here.
    @discardableResult
    func removeTab(tabID: UInt64) -> Bool {
        guard let index = hostedTabs.firstIndex(where: { $0.tabID == tabID }) else { return false }
        let hosted = hostedTabs.remove(at: index)
        hosted.state.isVisible = false
        hosted.state.isFocused = false
        hosted.hosting.willMove(toParent: nil)
        hosted.hosting.view.removeFromSuperview()
        hosted.hosting.removeFromParent()
        if index < selectedIndex || selectedIndex >= hostedTabs.count {
            selectedIndex = max(0, selectedIndex - 1)
        }
        applyTabStates()
        refreshStrip()
        return true
    }

    func select(tabID: UInt64) {
        guard let index = hostedTabs.firstIndex(where: { $0.tabID == tabID }), index != selectedIndex else { return }
        selectedIndex = index
        applyTabStates()
        refreshStrip()
    }

    func selectTab(atIndex index: Int) {
        guard hostedTabs.indices.contains(index) else { return }
        if index != selectedIndex {
            selectedIndex = index
            applyTabStates()
            refreshStrip()
        }
        selectionHandler?(hostedTabs[index].tabID)
    }

    /// For a restore, which gives focus separately.
    func setSelectedIndexWithoutFocus(_ index: Int) {
        guard hostedTabs.indices.contains(index), index != selectedIndex else { return }
        selectedIndex = index
        applyTabStates()
        refreshStrip()
    }

    func selectNextTab(step: Int) {
        guard hostedTabs.count > 1 else { return }
        selectTab(atIndex: (selectedIndex + step + hostedTabs.count) % hostedTabs.count)
    }

    func setWindowState(visible: Bool, focused: Bool) {
        windowIsVisible = visible
        windowHasFocus = focused
        applyTabStates()
    }

    /// How much of this window's bottom the keyboard covers, so the terminal's grid stops
    /// above it. The keyboard is the terminal's business here: Workspace itself doesn't
    /// shrink, because the other windows stay where they are.
    func setKeyboardOverlap(_ overlap: CGFloat) {
        contentBottomConstraint?.constant = -max(0, overlap)
    }

    private func applyTabStates() {
        for (index, hosted) in hostedTabs.enumerated() {
            let selected = index == selectedIndex
            hosted.hosting.view.isHidden = !selected
            let visible = selected && windowIsVisible
            let focused = visible && windowHasFocus
            if hosted.state.isVisible != visible {
                hosted.state.isVisible = visible
            }
            if hosted.state.isFocused != focused {
                hosted.state.isFocused = focused
            }
        }
    }

    private func refreshStrip() {
        guard isViewLoaded else { return }
        for subview in strip.arrangedSubviews {
            strip.removeArrangedSubview(subview)
            subview.removeFromSuperview()
        }
        let show = hostedTabs.count >= 2
        stripHeightConstraint?.constant = show ? Self.stripHeight : 0
        strip.isHidden = !show
        guard show else { return }
        for (index, hosted) in hostedTabs.enumerated() {
            let button = UIButton(type: .system)
            let selected = index == selectedIndex
            button.setTitle("\(index + 1)", for: .normal)
            button.titleLabel?.font = .monospacedDigitSystemFont(ofSize: 13, weight: selected ? .bold : .regular)
            button.tag = index
            button.layer.cornerRadius = 6
            button.layer.borderWidth = 1
            button.layer.borderColor = button.tintColor.withAlphaComponent(selected ? 0.9 : 0.3).cgColor
            button.backgroundColor = selected ? button.tintColor.withAlphaComponent(0.18) : .clear
            let title = TerminalTabManager.shared.tabTitle(tabId: hosted.tabID) ?? "Terminal"
            button.accessibilityLabel = "Tab \(index + 1), \(title)"
            button.accessibilityIdentifier = "workspace.tab.\(index + 1)"
            button.addTarget(self, action: #selector(tabButtonTapped(_:)), for: .touchUpInside)
            if !TerminalTabManager.shared.isRootTab(hosted.tabID) {
                let tabID = hosted.tabID
                button.menu = UIMenu(children: [
                    UIAction(title: "Close Tab", image: UIImage(systemName: "xmark")) { [weak self] _ in
                        self?.closeTabHandler?(tabID)
                    },
                ])
            }
            button.widthAnchor.constraint(greaterThanOrEqualToConstant: 34).isActive = true
            button.heightAnchor.constraint(equalToConstant: 22).isActive = true
            strip.addArrangedSubview(button)
        }
        let add = UIButton(type: .system)
        add.setImage(UIImage(systemName: "plus"), for: .normal)
        add.accessibilityLabel = "New Tab"
        add.accessibilityHint = "Opens a new terminal tab"
        add.accessibilityIdentifier = "workspace.tab.new"
        add.addTarget(self, action: #selector(newTabTapped), for: .touchUpInside)
        add.widthAnchor.constraint(equalToConstant: 30).isActive = true
        strip.addArrangedSubview(add)
        strip.addArrangedSubview(UIView())
    }

    @objc private func tabButtonTapped(_ button: UIButton) {
        selectTab(atIndex: button.tag)
    }

    @objc private func newTabTapped() {
        newTabHandler?()
    }
}
