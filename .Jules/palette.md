## 2024-05-22 - Transient Feedback for Non-Visual Actions
**Learning:** Actions like "Copy to Clipboard" often provide no visual confirmation, leaving users uncertain.
**Action:** Always add a transient "Copied!" state or toast notification for copy actions.

## 2024-05-22 - Accessibility Labels for Custom Keys
**Learning:** Custom input accessory views (keyboard rows) often lack accessibility labels by default.
**Action:** When implementing custom keyboard keys, always ensure `accessibilityLabel` is set for keys that use symbols (e.g., arrows, escape).

## 2024-05-23 - Accessibility Labels for Custom Keyboard Accessory Views
**Learning:** VoiceOver reads emoji literals (like "⚙️") rather than their intended action (like "Settings") if no `accessibilityLabel` is present. Abbreviated keys (like "Esc") also sound better when explicitly labeled ("Escape").
**Action:** When using emojis or abbreviated text as icons in custom toolbars or accessory views, explicitly set the `accessibilityLabel` to describe the action.
