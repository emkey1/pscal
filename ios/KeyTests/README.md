# KeyTests

Real keystrokes at the PSCAL app's real prompt, in an iOS simulator. Some behaviour only shows
with a person at the keys: lines typed while a program reads, Ctrl-C, Ctrl-D. `app_smoke_test.py`
can't type, and the iOS-flavoured host build has no session terminal, so neither can test these.
This can. It is not run by CI.

`keys.sh` has one case for each keyboard form fixed in V.1R33, and one for typed UTF-8 (V.1R34):

| Case | What it checks |
|---|---|
| `eof_readln_loop` | `while (!eof(stdin)) readln(stdin, s)` gets the typed lines, and Ctrl-D ends it |
| `eof_after_kill` | after a program waiting in `eof` is killed, the next one gets what is typed |
| `eof_ctrl_c_status` | Ctrl-C during `eof(stdin)` gives 130 |
| `readln_ctrl_c_quiet` | Ctrl-C on a Pascal `readln` gives 130 and prints no VM dump |
| `delay_ctrl_c` | Ctrl-C ends `delay()` at once |
| `wc_ctrl_c`, `sort_ctrl_c` | `wc` and `sort` print nothing on Ctrl-C |
| `pipe_ctrl_c` | Ctrl-C on `cat \| tr` gives 130, and the rest of the line doesn't run |
| `utf8_typed` | `echo é⚡x` and emoji typed at the prompt run whole, and Backspace takes an emoji whole (V.1R34) |

## Running it

You need Xcode, a booted or bootable iPad simulator, and a Release simulator build of the app:

```
SDK_NAME=iphonesimulator CONFIGURATION=Release bash ios/Tools/ensure_static_libs.sh
xcodebuild -project ios/PSCAL.xcodeproj -scheme PscalApp -configuration Release \
    -sdk iphonesimulator -destination 'generic/platform=iOS Simulator' \
    -derivedDataPath build/DerivedData-ios-sim CODE_SIGNING_ALLOWED=NO build
ios/KeyTests/keys.sh \
    --app build/DerivedData-ios-sim/Build/Products/Release-iphonesimulator/PSCAL.app \
    --device <simulator UDID>
```

Name cases to run only those; `--list` prints them. `--work DIR` sets where the driver is built
and where its command files and screenshots go (default `$TMPDIR/pscal-keytests`). Each check prints
`PASS` or `FAIL` with what it wanted and got, a failed check leaves a screenshot in `DIR/shots`, and
the script exits non-zero if any check failed. A run takes about ten minutes, since each case starts
the app afresh.

It was written against iPad simulators on iOS 26.5 and 27.2. It reinstalls the app, so whatever the
app held on that simulator is lost. Ctrl-<key> goes through the keyboard bar's Control key, and falls
back to a hardware keyboard's when the bar isn't there.

## How it works

`KeyTests.xcodeproj` builds an empty host app and `KeyDriver`, a UI test whose single test,
`testDrive`, runs until told to quit. It reads one command per file, `cmd.0`, `cmd.1`, ... in the
directory `KEYTESTS_DIR`, does it in the PSCAL app, and answers in `res.N`. `keys.sh` passes that
directory as `TEST_RUNNER_KEYTESTS_DIR` and writes the commands:

| Command | Does |
|---|---|
| `launch`, `activate`, `terminate` | the app |
| `text STRING` | types the string at once; `\n` is Return |
| `chars STRING` | types it a character at a time |
| `softkey NAME`, `tap NAME` | taps an on-screen key or a button by its accessibility name |
| `ctrl KEY` | a hardware keyboard's Control-KEY |
| `shot NAME` | a screenshot, `NAME.png` beside the command files |
| `tree`, `buttons`, `state`, `focus` | describes what is on screen |
| `quit` | ends the test |

To add a case, write a `case_NAME` function in `keys.sh` and add `NAME` to `ALL_CASES`. A case
should type, wait long enough for the app to act, and `check` a file the command wrote under `~/kt`.
