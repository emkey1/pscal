## 2024-05-22 - Transient Feedback for Non-Visual Actions
**Learning:** Actions like "Copy to Clipboard" often provide no visual confirmation, leaving users uncertain.
**Action:** Always add a transient "Copied!" state or toast notification for copy actions.

## 2024-05-22 - Accessibility Labels for Custom Keys
**Learning:** Custom input accessory views (keyboard rows) often lack accessibility labels by default.
**Action:** When implementing custom keyboard keys, always ensure `accessibilityLabel` is set for keys that use symbols (e.g., arrows, escape).
## 2024-05-24 - Missing State Feedback on Toggles and Inconsistent Labels
**Learning:** Discovered that the terminal's custom software accessory bar had a visually-toggling "Control" key that didn't expose its toggled state to screen readers (a common accessibility anti-pattern). Additionally, the settings button had inconsistent `accessibilityLabel`s across different shell tabs ("Adjust Font Size" vs "Terminal Settings") and lacked an `accessibilityHint`.
**Action:** Added `.selected` to `accessibilityTraits` dynamically when toggles are engaged. Unified the labels and added descriptive `accessibilityHint`s to icon-only buttons to clarify intent. This pattern of checking custom toggles for state broadcasting should be prioritized in future checks.
