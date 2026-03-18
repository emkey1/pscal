## 2024-05-22 - Transient Feedback for Non-Visual Actions
**Learning:** Actions like "Copy to Clipboard" often provide no visual confirmation, leaving users uncertain.
**Action:** Always add a transient "Copied!" state or toast notification for copy actions.

## 2024-05-22 - Accessibility Labels for Custom Keys
**Learning:** Custom input accessory views (keyboard rows) often lack accessibility labels by default.
**Action:** When implementing custom keyboard keys, always ensure `accessibilityLabel` is set for keys that use symbols (e.g., arrows, escape).

## 2024-05-24 - Missing State Feedback on Toggles and Inconsistent Labels
**Learning:** Discovered that the terminal's custom software accessory bar had a visually-toggling "Control" key that didn't expose its toggled state to screen readers (a common accessibility anti-pattern). Additionally, the settings button had inconsistent `accessibilityLabel`s across different shell tabs ("Adjust Font Size" vs "Terminal Settings") and lacked an `accessibilityHint`.
**Action:** Added `.selected` to `accessibilityTraits` dynamically when toggles are engaged. Unified the labels and added descriptive `accessibilityHint`s to icon-only buttons to clarify intent. This pattern of checking custom toggles for state broadcasting should be prioritized in future checks.

## 2025-02-18 - Missing Accessibility Hints for Custom Keyboard Accessory Keys
**Learning:** While custom keyboard accessory keys (like arrows, escape, or symbols) may have basic `accessibilityLabel`s, they often lack an `accessibilityHint`. For a terminal emulator, it's crucial that VoiceOver users understand what these specialized, symbol-only keys actually *do* (e.g., "Moves the cursor up or navigates history" vs just "Up Arrow").
**Action:** Added detailed `accessibilityHint`s to all icon-only buttons in the terminal accessory bar. Always ensure that `accessibilityHint`s are provided for symbol-based keys that perform complex terminal operations.

## 2025-02-18 - Missing Accessibility Hints on Secondary Options and Dynamic Buttons
**Learning:** Utilities and supplementary views often have buttons with ambiguous labels or text that dynamically changes upon action (e.g., "Copied!" for 2 seconds). Screen readers can miss the intent of "Use Documents root" or fail to announce state changes clearly.
**Action:** Always add explicit `accessibilityHint`s describing the outcome of secondary buttons (e.g., "Resets the path truncation..."). For transient state changes, use dynamic `accessibilityLabel`s (e.g., `.accessibilityLabel(copiedColors ? "Copied" : "Copy")`) to ensure the state update is accessible.

## 2025-02-21 - Missing Accessibility Hints on SwiftUI Toggles with External Explanatory Text
**Learning:** Many SwiftUI `Toggle` elements in settings screens have standard labels but rely on trailing or underlying `Text` views (e.g., footnotes) for explanation. Screen reader users who navigate directly to the toggle via swipe or rotor might hear the label ("Restore on app start") but miss the critical context provided visually below it ("Used for this tab profile. Enable restore to apply...").
**Action:** Always append `.accessibilityHint("Brief description of what enabling/disabling this does")` to `Toggle` components when their implications are not immediately obvious from the label alone, effectively embedding the visual footnote text into the screen reader's announcement for that interactive element.
