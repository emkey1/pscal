// Copyright 2026 Mike Miller
// SPDX-License-Identifier: Apache-2.0
//
// Ported by its author from iSH-AOK's Workspace (app/WorkspaceViewController.m at
// 0f75fee1c) and licensed to PSCAL under the Apache License 2.0.

import Foundation
import QuartzCore
import UIKit

/// The shell's side of Workspace, behind smallclue's `ws` command. Two questions a shell
/// couldn't otherwise ask: "am I under Workspace?" and "will you open this for me?". It's
/// deliberately narrow: one verb, a fixed set of tools, and an absolute path.
///
/// These run on a shell's thread, whose terminal can be on screen. Waiting on the main thread
/// without a limit is how that deadlocks, so an open returns once it's accepted, and a list
/// waits a bounded time and otherwise answers with the last one built.
enum WorkspaceBridge {
    private static let lock = NSLock()
    private nonisolated(unsafe) static var hostedTools: [String]?
    private nonisolated(unsafe) static var lastApplets = ""

    /// Workspace came on screen, with these tools to offer, or left it.
    static func setHosted(tools: [String]?) {
        lock.lock()
        hostedTools = tools
        lock.unlock()
    }

    private static func currentTools() -> [String]? {
        lock.lock()
        defer { lock.unlock() }
        return hostedTools
    }

    static func statusText() -> String {
        guard let tools = currentTools() else {
            return "hosted=0\nreason=this session is not under Workspace\n"
        }
        return "hosted=1\ntools=\(tools.joined(separator: ","))\nverbs=open,applets\n"
    }

    /// 0 or a negative errno. Success means accepted: whether a window can actually be shown
    /// depends on state only the main thread can see.
    static func open(tool: String, path: String?) -> Int32 {
        guard let tools = currentTools() else { return -EOPNOTSUPP }
        // Refused here, so the shell gets a real error rather than silence from a queue.
        guard tools.contains(tool) else { return -EINVAL }
        DispatchQueue.main.async {
            MainActor.assumeIsolated {
                // The check that counts: Workspace may have gone since the request.
                TerminalRootViewController.activeInstance?.workspace?.open(tool: tool, path: path)
            }
        }
        return 0
    }

    @MainActor
    private static func buildApplets() -> String {
        let body = TerminalRootViewController.activeInstance?.workspace?.appletListText() ?? ""
        lock.lock()
        lastApplets = body
        lock.unlock()
        return body
    }

    static func appletsText() -> String {
        if Thread.isMainThread {
            return MainActor.assumeIsolated { buildApplets() }
        }
        let built = DispatchSemaphore(value: 0)
        let box = StringBox()
        DispatchQueue.main.async {
            MainActor.assumeIsolated {
                box.set(buildApplets())
                built.signal()
            }
        }
        if built.wait(timeout: .now() + .milliseconds(500)) == .success, let fresh = box.get() {
            return fresh
        }
        lock.lock()
        defer { lock.unlock() }
        return lastApplets
    }

    private final class StringBox: @unchecked Sendable {
        private let lock = NSLock()
        private var value: String?

        func set(_ text: String) {
            lock.lock()
            value = text
            lock.unlock()
        }

        func get() -> String? {
            lock.lock()
            defer { lock.unlock() }
            return value
        }
    }
}

extension WorkspaceViewController {
    /// `ws open`. A document applet gets a window of its own for each path. Any other keeps to
    /// a menu's rule, one window that comes forward, and takes the path there if it can.
    func open(tool: String, path: String?) {
        guard let path else {
            openOrFocusTool(tool)
            return
        }
        let window = WorkspaceToolRegistry.opensFiles(tool) ? openTool(tool) : openOrFocusTool(tool)
        guard let window, let applet = applet(of: window) as? WorkspaceFileOpenable else { return }
        applet.workspaceOpenFile(atPath: path)
    }

    /// One line per applet window, in the order they opened: id, tool, Desktop or * for all,
    /// front, shown or hidden, age in seconds, title.
    func appletListText() -> String {
        let front = frontmostWindow()
        let now = CACurrentMediaTime()
        var lines = ""
        for window in desktopWindows.sorted(by: { $0.serial < $1.serial }) {
            guard let tool = window.toolIdentifier else { continue }
            let desktop = WorkspaceToolRegistry.isGlobal(tool) ? "*" : String(window.desktopIndex + 1)
            let state = window === front ? "front" : (window.isHidden ? "hidden" : "shown")
            let title = (window.titleLabel.text ?? "").replacingOccurrences(of: "\n", with: " ")
            lines += "\(window.serial) \(tool) \(desktop) \(state) \(Int(max(0, now - window.openedAt))) \(title.isEmpty ? "-" : title)\n"
        }
        return lines
    }
}

@_cdecl("pscalRuntimeWorkspaceStatus")
func pscalRuntimeWorkspaceStatus() -> UnsafeMutablePointer<CChar>? {
    strdup(WorkspaceBridge.statusText())
}

@_cdecl("pscalRuntimeWorkspaceApplets")
func pscalRuntimeWorkspaceApplets() -> UnsafeMutablePointer<CChar>? {
    strdup(WorkspaceBridge.appletsText())
}

@_cdecl("pscalRuntimeWorkspaceOpen")
func pscalRuntimeWorkspaceOpen(_ tool: UnsafePointer<CChar>?, _ path: UnsafePointer<CChar>?) -> Int32 {
    guard let tool else { return -EINVAL }
    return WorkspaceBridge.open(tool: String(cString: tool), path: path.map { String(cString: $0) })
}
