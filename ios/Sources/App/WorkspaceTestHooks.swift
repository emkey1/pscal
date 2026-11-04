// Copyright 2026 Mike Miller
// SPDX-License-Identifier: Apache-2.0
//
// Ported by its author from iSH-AOK's Workspace (app/WorkspaceTestHooks.m at 0f75fee1c)
// and licensed to PSCAL under the Apache License 2.0.

import Foundation
import UIKit

/// A remote control for Workspace, so that it can be tested end to end without touching
/// the screen. `ios/Tools/workspace_test.py` drives it.
///
/// The channel is a directory and a Darwin notification. The harness writes one command
/// line to `<app data>/tmp/workspace-test/cmd.txt`, then posts the notification
/// (`simctl spawn ... notifyutil -p`). The app answers in `result-<seq>.json` beside it. A
/// command line is `<seq> <verb> [args...]`. Windows are named by the `id` a dump gave them.
///
/// Verbs:
///   dump                           windows, tabs, focus, and whether each terminal is attached
///   workspace                      enter Workspace
///   tabs                           back to the tab bar
///   open-terminal                  New Terminal, as the menu does it
///   new-tab <id>                   New Tab in that window
///   select-tab <id> <index>        show that tab of that window (from 0)
///   close <id>                     the window's ×
///   close-tab <tab>                close one session
///   focus <id>                     bring that window forward
///   focus-desktop                  tap the bare desktop
///   frame <id> <x> <y> <w> <h>     place a window, in fractions of the desktop
///   zoom <id>                      what a double tap on the title bar does
///   type <tab> <text>              send text to a session (\n \r \t \\ decoded)
///   select-session <tab>           make that session the manager's selected one
///   open-tool <identifier>         open an applet's window
///   desktops <n>                   have n Desktops
///   assign <id> <desktop>          move a window to a Desktop (from 0)
///   switch <desktop>               show a Desktop
///   remove-desktop <desktop>       what the Desktops applet's × does
///   save-arrangement               the Desktops applet's Save
///   restore-arrangement            the Desktops applet's Restore
///   open-or-focus <identifier>     what a menu does with an applet
///   text-scale <id> <step>         Cmd+= (1), Cmd+- (-1) or Cmd+0 (0) for that window's applet
///   theme <identifier>             choose a theme
///   gauge <ring|bar>               choose the gauge style
///   density <0..1>                 set the density
///   run-shortcut <command>         what tapping a Launcher shortcut does
///   add-shortcut <name> <command>  add a Launcher shortcut at the top level (command may be empty)
///
/// Off unless PSCALI_WORKSPACE_TEST=1 is in the app's environment.
@MainActor
enum WorkspaceTestHooks {
    private static let notificationName = "com.pscal.PSCAL.workspace-test"
    private static var installed = false

    private static var directory: URL {
        URL(fileURLWithPath: NSTemporaryDirectory()).appendingPathComponent("workspace-test", isDirectory: true)
    }

    static var isEnabled: Bool {
        ProcessInfo.processInfo.environment["PSCALI_WORKSPACE_TEST"] == "1"
    }

    static func installIfEnabled() {
        guard isEnabled, !installed else { return }
        installed = true
        try? FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        CFNotificationCenterAddObserver(CFNotificationCenterGetDarwinNotifyCenter(),
                                        nil,
                                        { _, _, _, _, _ in
                                            DispatchQueue.main.async {
                                                MainActor.assumeIsolated {
                                                    WorkspaceTestHooks.handleCommandFile()
                                                }
                                            }
                                        },
                                        notificationName as CFString,
                                        nil,
                                        .deliverImmediately)
        write(result: ["ready": true], name: "ready.json")
    }

    private static func handleCommandFile() {
        let file = directory.appendingPathComponent("cmd.txt")
        guard let line = try? String(contentsOf: file, encoding: .utf8) else { return }
        try? FileManager.default.removeItem(at: file)
        let trimmed = line.trimmingCharacters(in: .newlines)
        var parts = trimmed.split(separator: " ", maxSplits: 2, omittingEmptySubsequences: false).map(String.init)
        guard parts.count >= 2 else { return }
        let seq = parts.removeFirst()
        let verb = parts.removeFirst()
        let rest = parts.first ?? ""
        let result = run(verb: verb, rest: rest)
        write(result: result, name: "result-\(seq).json")
    }

    private static func write(result: [String: Any], name: String) {
        guard JSONSerialization.isValidJSONObject(result),
              let data = try? JSONSerialization.data(withJSONObject: result, options: [.sortedKeys]) else {
            return
        }
        let target = directory.appendingPathComponent(name)
        try? data.write(to: target, options: .atomic)
    }

    private static func decode(_ text: String) -> String {
        var out = ""
        var escaped = false
        for character in text {
            if escaped {
                switch character {
                case "n": out.append("\n")
                case "r": out.append("\r")
                case "t": out.append("\t")
                case "\\": out.append("\\")
                default: out.append(character)
                }
                escaped = false
            } else if character == "\\" {
                escaped = true
            } else {
                out.append(character)
            }
        }
        return out
    }

    private static func run(verb: String, rest: String) -> [String: Any] {
        let manager = TerminalTabManager.shared
        guard let root = TerminalRootViewController.activeInstance else {
            return ["ok": false, "error": "no root view controller"]
        }
        let args = rest.split(separator: " ").map(String.init)
        let workspace = root.workspace
        func window(_ text: String?) -> WorkspaceWindowView? {
            guard let text, let serial = Int(text) else { return nil }
            return workspace?.desktopWindows.first { $0.serial == serial }
        }
        switch verb {
        case "dump":
            return dump()
        case "workspace":
            root.enterWorkspace()
        case "tabs":
            root.leaveWorkspace()
        case "open-terminal":
            guard let workspace else { return ["ok": false, "error": "not in Workspace"] }
            workspace.openNewTerminalWindow()
        case "new-tab":
            guard let workspace, let target = window(args.first) else { return ["ok": false, "error": "no such window"] }
            workspace.openNewTab(in: target)
        case "select-tab":
            guard let target = window(args.first), args.count > 1, let index = Int(args[1]),
                  let content = workspace?.content(of: target) else {
                return ["ok": false, "error": "no such window or tab"]
            }
            content.selectTab(atIndex: index)
        case "close":
            guard let workspace, let target = window(args.first) else { return ["ok": false, "error": "no such window"] }
            workspace.closeWindow(target)
        case "close-tab":
            guard let text = args.first, let tabID = UInt64(text) else { return ["ok": false, "error": "no such tab"] }
            manager.closeTab(id: tabID)
            workspace?.syncFromManager()
        case "focus":
            guard let workspace, let target = window(args.first) else { return ["ok": false, "error": "no such window"] }
            workspace.focus(window: target)
        case "focus-desktop":
            workspace?.focusDesktopForTesting()
        case "frame":
            guard let workspace, let target = window(args.first), args.count == 5,
                  let x = Double(args[1]), let y = Double(args[2]), let w = Double(args[3]), let h = Double(args[4]) else {
                return ["ok": false, "error": "usage: frame <id> <x> <y> <w> <h>"]
            }
            workspace.setWindowFrameForTesting(target, fractions: CGRect(x: x, y: y, width: w, height: h))
        case "zoom":
            guard let target = window(args.first) else { return ["ok": false, "error": "no such window"] }
            target.toggleZoom()
        case "type":
            let pieces = rest.split(separator: " ", maxSplits: 1, omittingEmptySubsequences: false).map(String.init)
            guard pieces.count == 2, let tabID = UInt64(pieces[0]),
                  let tab = manager.tabs.first(where: { $0.id == tabID }) else {
                return ["ok": false, "error": "no such tab"]
            }
            let text = decode(pieces[1])
            switch tab.kind {
            case .shell(let runtime):
                runtime.send(text)
            case .shellSession(let session), .ssh(let session):
                session.send(text)
            case .sdl:
                return ["ok": false, "error": "not a terminal"]
            }
        case "select-session":
            guard let text = args.first, let tabID = UInt64(text) else { return ["ok": false, "error": "no such tab"] }
            manager.selectedId = tabID
        case "open-tool":
            guard let workspace, let identifier = args.first, workspace.openTool(identifier) != nil else {
                return ["ok": false, "error": "no such applet"]
            }
        case "desktops":
            guard let workspace, let text = args.first, let count = Int(text) else { return ["ok": false, "error": "usage: desktops <n>"] }
            workspace.setDesktopCountForTesting(count)
        case "assign":
            guard let workspace, let target = window(args.first), args.count > 1, let index = Int(args[1]) else {
                return ["ok": false, "error": "usage: assign <id> <desktop>"]
            }
            workspace.assignWindow(target, toDesktop: index)
        case "switch":
            guard let workspace, let text = args.first, let index = Int(text) else { return ["ok": false, "error": "usage: switch <desktop>"] }
            workspace.switchToDesktop(index)
        case "remove-desktop":
            guard let workspace, let text = args.first, let index = Int(text) else { return ["ok": false, "error": "usage: remove-desktop <desktop>"] }
            workspace.removeDesktop(index)
        case "save-arrangement":
            guard let workspace else { return ["ok": false, "error": "not in Workspace"] }
            workspace.saveArrangement()
        case "restore-arrangement":
            guard let workspace else { return ["ok": false, "error": "not in Workspace"] }
            workspace.restoreArrangementFromButton()
        case "open-or-focus":
            guard let workspace, let identifier = args.first, workspace.openOrFocusTool(identifier) != nil else {
                return ["ok": false, "error": "no such applet"]
            }
        case "text-scale":
            guard let workspace, let target = window(args.first), args.count > 1, let step = Int(args[1]),
                  let applet = workspace.applet(of: target), applet is WorkspaceTextScalable else {
                return ["ok": false, "error": "no scalable applet in that window"]
            }
            applet.workspaceTextScale = WorkspaceTextScale.stepped(applet.workspaceTextScale, step: step)
        case "theme":
            guard let identifier = args.first, WorkspaceTheme.record(forIdentifier: identifier) != nil else {
                return ["ok": false, "error": "no such theme"]
            }
            WorkspaceTheme.setCurrentIdentifier(identifier)
        case "gauge":
            WorkspaceTheme.usesRingGauges = args.first != "bar"
        case "density":
            guard let text = args.first, let value = Double(text) else { return ["ok": false, "error": "usage: density <0..1>"] }
            WorkspaceTheme.density = CGFloat(value)
        case "run-shortcut":
            guard let workspace else { return ["ok": false, "error": "not in Workspace"] }
            workspace.runLauncherShortcut(command: decode(rest), title: nil)
        case "add-shortcut":
            let pieces = rest.split(separator: " ", maxSplits: 1, omittingEmptySubsequences: false).map(String.init)
            guard let name = pieces.first, !name.isEmpty else { return ["ok": false, "error": "usage: add-shortcut <name> <command>"] }
            let command = pieces.count > 1 ? decode(pieces[1]) : ""
            WorkspaceLauncherStore.mutate(atPath: []) { $0.append(["name": name, "command": command]) }
        default:
            return ["ok": false, "error": "unknown verb \(verb)"]
        }
        return ["ok": true]
    }

    private static func htermController(_ tab: TerminalTabManager.Tab) -> HtermTerminalController? {
        switch tab.kind {
        case .shell(let runtime):
            return runtime.htermControllerIfCreated()
        case .shellSession(let session), .ssh(let session):
            return session.htermController
        case .sdl:
            return nil
        }
    }

    private static func kindName(_ tab: TerminalTabManager.Tab) -> String {
        switch tab.kind {
        case .shell: return "shell"
        case .shellSession: return "shellSession"
        case .ssh: return "ssh"
        case .sdl: return "sdl"
        }
    }

    private static func tabDescriptor(_ tab: TerminalTabManager.Tab) -> [String: Any] {
        let controller = htermController(tab)
        return [
            "id": String(tab.id),
            "title": tab.title,
            "kind": kindName(tab),
            "root": TerminalTabManager.shared.isRootTab(tab.id),
            "webAttached": controller?.webView.window != nil,
            "loaded": controller?.isLoaded ?? false,
        ]
    }

    private static func dump() -> [String: Any] {
        let manager = TerminalTabManager.shared
        guard let root = TerminalRootViewController.activeInstance else {
            return ["ok": false, "error": "no root view controller"]
        }
        var result: [String: Any] = [
            "ok": true,
            "mode": root.workspace == nil ? "tabs" : "workspace",
            "selectedId": String(manager.selectedId),
            "tabs": manager.tabs.map(tabDescriptor),
            "keyInputFocused": root.view.window.flatMap { keyInputFirstResponder(in: $0) }.map { _ in true } ?? false,
        ]
        guard let workspace = root.workspace else { return result }
        let front = workspace.frontmostWindow()
        result["surface"] = [workspace.desktopSizeForTesting.width, workspace.desktopSizeForTesting.height]
        result["activeDesktop"] = workspace.activeDesktopIndex
        result["desktopCount"] = workspace.desktopCount
        result["saved"] = workspace.arrangementIsSaved
        result["theme"] = WorkspaceTheme.currentIdentifier
        result["hasWallpaper"] = workspace.hasWallpaperForTesting
        result["shortcuts"] = WorkspaceLauncherStore.shortcuts.count
        result["windows"] = workspace.desktopWindows.map { window -> [String: Any] in
            let content = workspace.content(of: window)
            return [
                "id": window.serial,
                "textScale": Double(workspace.applet(of: window)?.workspaceTextScale ?? 1),
                "title": window.titleLabel.text ?? "",
                "tool": window.toolIdentifier ?? "",
                "desktop": window.desktopIndex,
                "hidden": window.isHidden,
                "frame": [window.frame.minX, window.frame.minY, window.frame.width, window.frame.height],
                "front": window === front,
                "focused": window === workspace.focusedWindowForTesting,
                "zoomed": window.zoomedToFullscreen,
                "closable": !window.closeButton.isHidden,
                "selectedTab": content?.selectedTabID.map { String($0) } ?? "",
                "tabs": (content?.hostedTabs ?? []).map { hosted -> [String: Any] in
                    [
                        "id": String(hosted.tabID),
                        "visible": hosted.state.isVisible,
                        "focused": hosted.state.isFocused,
                    ]
                },
            ]
        }
        return result
    }

    private static func keyInputFirstResponder(in view: UIView) -> TerminalKeyInputView? {
        if let input = view as? TerminalKeyInputView, input.isFirstResponder {
            return input
        }
        for subview in view.subviews {
            if let found = keyInputFirstResponder(in: subview) {
                return found
            }
        }
        return nil
    }
}
