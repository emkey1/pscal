// Copyright 2026 Mike Miller
// SPDX-License-Identifier: Apache-2.0

import XCTest

/// What the file-driven hooks in WorkspaceTestHooks.swift can't reach: real touches and real
/// keys. Everything else about Workspace is in ios/Tools/workspace_test.py.
final class WorkspaceUITests: XCTestCase {
    private var app: XCUIApplication!

    private var menuButton: XCUIElement { app.buttons["workspace.menu"] }

    private var windows: XCUIElementQuery {
        app.otherElements.matching(NSPredicate(format: "identifier MATCHES 'workspace\\\\.window\\\\.[0-9]+'"))
    }

    override func setUpWithError() throws {
        continueAfterFailure = false
        app = XCUIApplication()
        app.launchEnvironment["PSCALI_WORKSPACE_START"] = "1"
        app.launch()
        XCTAssertTrue(menuButton.waitForExistence(timeout: 60), "Workspace didn't come up")
        XCTAssertTrue(windows.firstMatch.waitForExistence(timeout: 30), "no window on the desktop")
    }

    /// By its label, but not the tab bar's own Workspace button, which is there underneath.
    private func tap(_ title: String) {
        let button = app.buttons.matching(NSPredicate(format: "label == %@ AND identifier != 'workspace.enter'", title)).firstMatch
        XCTAssertTrue(button.waitForExistence(timeout: 10), "no \(title) in the menu")
        button.tap()
    }

    private func openApplet(group: String, title: String) -> XCUIElement {
        menuButton.tap()
        tap("Applets")
        tap(group)
        tap(title)
        let window = windows.containing(.staticText, identifier: title).firstMatch
        XCTAssertTrue(window.waitForExistence(timeout: 10), "\(title) didn't open")
        return window
    }

    private func drag(_ window: XCUIElement, from offset: CGVector, by delta: CGVector) {
        let start = window.coordinate(withNormalizedOffset: .zero).withOffset(offset)
        start.press(forDuration: 0.3, thenDragTo: start.withOffset(delta))
    }

    func testTheMenuOpensAnAppletAndItsButtonClosesIt() {
        let before = windows.count
        let clock = openApplet(group: "Status", title: "Clock")
        XCTAssertEqual(windows.count, before + 1)
        clock.buttons["workspace.window.close"].tap()
        XCTAssertFalse(clock.waitForExistence(timeout: 2))
        XCTAssertEqual(windows.count, before)
    }

    func testDraggingTheTitleBarMovesAWindow() {
        let clock = openApplet(group: "Status", title: "Clock")
        let before = clock.frame
        drag(clock, from: CGVector(dx: before.width / 2, dy: 12), by: CGVector(dx: 30, dy: 80))
        let after = clock.frame
        // The gesture takes some of the distance to start, so not the whole 80 points.
        XCTAssertGreaterThan(after.minY - before.minY, 40)
        XCTAssertEqual(after.width, before.width, accuracy: 1)
        XCTAssertEqual(after.height, before.height, accuracy: 1)
    }

    func testDraggingTheCornerResizesAWindow() {
        let clock = openApplet(group: "Status", title: "Clock")
        let before = clock.frame
        drag(clock, from: CGVector(dx: before.width - 14, dy: before.height - 14), by: CGVector(dx: 40, dy: 60))
        let after = clock.frame
        XCTAssertEqual(after.minX, before.minX, accuracy: 1)
        XCTAssertGreaterThan(after.width, before.width + 10)
        XCTAssertGreaterThan(after.height, before.height + 10)
    }

    /// MotePad's text view in that window, empty: a saved arrangement can bring others back,
    /// and an unsaved draft returns in the next new window.
    private func emptyText(in pad: XCUIElement) -> XCUIElement {
        let text = pad.textViews["workspace.motepad.text"]
        XCTAssertTrue(text.waitForExistence(timeout: 10))
        text.tap()
        text.typeKey("a", modifierFlags: .command)
        text.typeText(XCUIKeyboardKey.delete.rawValue)
        return text
    }

    func testCommandWInMotePadAsksBeforeLosingWork() {
        let pad = openApplet(group: "Workspace", title: "MotePad")
        let text = emptyText(in: pad)
        text.typeText("worth keeping")
        app.typeKey("w", modifierFlags: .command)
        let alert = app.alerts["Unsaved Changes"]
        XCTAssertTrue(alert.waitForExistence(timeout: 5), "Cmd-W closed MotePad without asking")
        XCTAssertTrue(pad.exists)
        alert.buttons["Don’t Save"].tap()
        XCTAssertFalse(pad.waitForExistence(timeout: 2))
    }

    func testCommandArrowsMoveTheCaretInMotePadNotTheDesktop() {
        let text = emptyText(in: openApplet(group: "Workspace", title: "MotePad"))
        text.typeText("ab")
        app.typeKey(.leftArrow, modifierFlags: .command)
        text.typeText("c")
        XCTAssertEqual(text.value as? String, "cab")
    }
}
