## Smallclue Applets on iPadOS/iOS

Workflow reminders for exposing a new `smallclue` command inside the iPad/iPhone shell (`exsh`):

1. **Implement the applet** in `src/smallclue/core.c`:
   - Add it to `kSmallclueApplets`.
   - Supply a short description (used by `smallclue --help`), and keep the array sorted alphabetically.
2. **Bridge it into the VM** by:
   - Adding a `DEFINE_SMALLCLUE_WRAPPER("name", ident)` entry in `src/smallclue/integration.c`.
   - Registering the builtin with `registerVmBuiltin` **inside the `#if defined(PSCAL_TARGET_IOS)` block** so it overrides the legacy DOS builtin on iOS.
3. **Expose it to the shell command table** by appending `{ "name", "canonical", -1 }` to the iOS block in `src/shell/builtins.c` (keep this block sorted too).
4. Rebuild/run `exsh --dump-ext-builtins` to confirm the command shows up, and run `smallclue --help` to verify the usage list matches.

Following all these steps prevents the regressions we’ve seen (command works on desktop but not on iPadOS) and keeps the help text accurate.***
