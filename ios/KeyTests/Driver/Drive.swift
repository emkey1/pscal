import XCTest

/// Obeys <root>/cmd.N in order and answers in <root>/res.N, until told to quit. keys.sh writes the
/// commands; <root> comes from KEYTESTS_DIR, which xcodebuild passes on as TEST_RUNNER_KEYTESTS_DIR.
final class Drive: XCTestCase {
    let root = ProcessInfo.processInfo.environment["KEYTESTS_DIR"] ?? NSTemporaryDirectory() + "keytests"

    func unescape(_ text: String) -> String {
        text.replacingOccurrences(of: "\\n", with: "\n")
            .replacingOccurrences(of: "\\b", with: "\u{8}")
            .replacingOccurrences(of: "\\e", with: "\u{1b}")
    }

    func testDrive() throws {
        continueAfterFailure = true
        let app = XCUIApplication(bundleIdentifier: "com.pscal.PSCAL")
        var n = 0
        while true {
            let path = "\(root)/cmd.\(n)"
            guard FileManager.default.fileExists(atPath: path) else {
                Thread.sleep(forTimeInterval: 0.1)
                continue
            }
            let line = try String(contentsOfFile: path, encoding: .utf8).trimmingCharacters(in: .newlines)
            let parts = line.split(separator: " ", maxSplits: 1, omittingEmptySubsequences: false).map(String.init)
            let arg = parts.count > 1 ? parts[1] : ""
            var out = "ok"
            switch parts[0] {
            case "launch":
                app.launch()
            case "activate":
                app.activate()
            case "terminate":
                app.terminate()
            case "text":
                // A whole string at once, as a paste or fast typing would arrive.
                app.typeText(unescape(arg))
            case "chars":
                for c in unescape(arg) {
                    app.typeText(String(c))
                    Thread.sleep(forTimeInterval: 0.05)
                }
            case "softkeys":
                // Taps on the on-screen keyboard, when one is showing.
                let keyboard = app.keyboards.firstMatch
                if !keyboard.exists {
                    out = "no on-screen keyboard"
                    break
                }
                for c in arg {
                    let name = c == " " ? "space" : String(c)
                    let key = keyboard.keys[name].firstMatch
                    if key.exists {
                        key.tap()
                    } else {
                        out = "no key \(name)"
                    }
                }
            case "softkey":
                // Any on-screen key by its accessibility name, such as delete.
                let key = app.keys[arg].firstMatch
                if key.exists {
                    key.tap()
                } else {
                    out = "no key \(arg)"
                }
            case "softreturn":
                let key = app.keyboards.firstMatch.buttons["return"].firstMatch
                if key.exists {
                    key.tap()
                } else {
                    out = "no return key"
                }
            case "ctrl":
                app.typeKey(arg, modifierFlags: .control)
            case "key":
                let keys: [String: String] = [
                    "up": XCUIKeyboardKey.upArrow.rawValue, "down": XCUIKeyboardKey.downArrow.rawValue,
                    "left": XCUIKeyboardKey.leftArrow.rawValue, "right": XCUIKeyboardKey.rightArrow.rawValue,
                    "delete": XCUIKeyboardKey.delete.rawValue, "tab": XCUIKeyboardKey.tab.rawValue,
                    "escape": XCUIKeyboardKey.escape.rawValue, "return": XCUIKeyboardKey.return.rawValue,
                ]
                app.typeKey(keys[arg] ?? arg, modifierFlags: [])
            case "cmdkey":
                app.typeKey(arg, modifierFlags: .command)
            case "tap":
                let button = app.buttons[arg].firstMatch
                if button.waitForExistence(timeout: 5) {
                    button.tap()
                } else {
                    out = "no button \(arg)"
                }
            case "xy":
                let nums = arg.split(separator: " ").compactMap { Double($0) }
                app.coordinate(withNormalizedOffset: CGVector(dx: 0, dy: 0))
                    .withOffset(CGVector(dx: nums[0], dy: nums[1])).tap()
            case "buttons":
                out = app.buttons.allElementsBoundByIndex.map { "\($0.label)|\($0.identifier)" }.joined(separator: "\n")
            case "tree":
                out = app.debugDescription
            case "shot":
                try XCUIScreen.main.screenshot().pngRepresentation
                    .write(to: URL(fileURLWithPath: "\(root)/\(arg).png"))
            case "state":
                out = "\(app.state.rawValue)"
            case "focus":
                out = "keyboard focus: " + (app.textViews.allElementsBoundByIndex
                    .map { "\($0.hasFocus)" }.joined(separator: ","))
            case "quit":
                try out.write(toFile: "\(root)/res.\(n)", atomically: true, encoding: .utf8)
                return
            default:
                out = "unknown command"
            }
            try out.write(toFile: "\(root)/res.\(n)", atomically: true, encoding: .utf8)
            n += 1
        }
    }
}
