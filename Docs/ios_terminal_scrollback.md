# iOS terminal scrollback and memory

The iOS/iPadOS app renders the terminal with hterm inside a `WKWebView`
(`ios/Sources/App/TerminalWeb/`). hterm keeps every row as a DOM node, and rows
that scroll off the top of the screen are moved into `hterm.Terminal`'s
`scrollbackRows_` array as *detached* `<x-row>` elements. A row holds one node
per run of text attributes within it, so coloured output costs roughly seven
nodes per line rather than one.

The vendored `hterm_all.js` has no scrollback limit, which made this the app's
dominant memory leak: the app was killed by jetsam after an hour or two of use.
Two separate mechanisms fed it.

## 1. Unbounded primary scrollback

Nothing trimmed `scrollbackRows_`. The only reset was `clearScrollback()`, which
the app never called, so every line ever printed stayed resident for the life of
the session.

Measured in WebKit with 60-column plain-text lines:

| lines written | rows retained | retained row text |
|---|---|---|
| 50,000 | 49,978 | 3.0 MB |
| 200,000 | 199,978 | 12.0 MB |

The row text is the small part. In the simulator, 200,000 lines of plain digits
grew the web content process from 170 MB to 279 MB and it stayed there — about
560 bytes of process memory per retained row, for the cheapest output there is.

## 2. Alternate-screen rows were retained

Worse, rows scrolled off the *alternate* screen were pushed into the scrollback
too. Real terminals discard them: the alternate screen has no scrollback. Because
full-screen applications (`vi`, `micro`, `htop`, `less`, dvtm) repaint their whole
screen continuously, sitting in one accumulated a permanent scrollback row per
repainted line — and the garbage survived after the application exited. 30,000
lines of pager repaint added 30,000 rows to the user's scrollback.

This is the mechanism that made the app die while apparently idle, and it is why
the caps on the Swift side never helped: `TerminalBuffer` (which suppresses
scrollback tracking on the alternate screen, and caps at 400 rows) is the
*fallback* renderer. hterm is what actually draws the terminal.

## The fix

`hterm_all.js` gained `pushScrollbackRows_()`, which every scroll-off site now
routes through (`realizeHeight_`, `appendRows_`, `insertRow_`). It drops rows
from the alternate screen and otherwise appends and then calls
`trimScrollback_()`.

`trimScrollback_()` drops the oldest rows once the buffer runs past
`scrollbackLimit_`. Rows are addressed by absolute index, so dropping from the
front shifts every index that remains; the trim renumbers the surviving
scrollback rows and both screens, resets the ScrollPort's row cache, releases a
selection that reached into the dropped rows, and re-pins the viewport (to the
bottom if it was pinned there, otherwise to the same content). Because
renumbering is O(scrollback), rows are dropped in batches of
`SCROLLBACK_TRIM_SLACK` to keep the amortized cost per line flat — measured at a
2% throughput cost over 100,000 lines.

`term.js` sets the policy: `term.setScrollbackLimit(5000)`. The limit is also
reachable at runtime as `exports.setScrollbackLimit(rows)` if it ever needs to
become a preference, and `exports.scrollbackRowCount()` reports the current
depth. `setScrollbackLimit(0)` restores the old unbounded behaviour.

Result for the same 200,000-line run in the simulator:

| | before | after |
|---|---|---|
| web content baseline | 170 MB | 170 MB |
| peak during the run | 342 MB | 225 MB |
| settled after the run | 279 MB | 141 MB |
| retained | **+109 MB** | **none** |

## Testing

`Tests/run_ios_terminalweb_tests.sh` (run by `Tests/run_ios_port_tests.sh`) is a
wiring guard, not a behavioural test: it checks the patch is still present and
that no scroll-off site bypasses `pushScrollbackRows_`. It exists because
`hterm_all.js` is vendored and re-vendoring would silently drop the cap.

Behaviour needs a real WebKit — hterm builds its screen inside an iframe and
needs layout, so the row nodes only exist when the page is actually sized. To
check it by hand, serve `ios/Sources/App/TerminalWeb/` over HTTP with a page that
stubs `window.webkit.messageHandlers` (term.js posts to `load`, `log`,
`sendInput`, `resize`, `propUpdate`, `syncFocus`, `focus`, `newScrollHeight`,
`newScrollTop`, `openLink`, `selectionChanged`), give `#terminal` an explicit
pixel size, then drive `exports.write()` and read `term.scrollbackRows_.length`.
Rows render into `term.scrollPort_.getDocument()`, not the top-level document.

Worth re-checking after any hterm bump:

- scrollback settles at the limit (plus up to the trim slack) under sustained output
- alternate-screen output adds nothing, and primary scrollback survives the round trip
- rendered rows still match `term.getRowText()` while scrolled back into history
- `scrollPort_.invalidate()` does not throw after a trim (stale `rowIndex` values
  make `drawVisibleRows_` throw `Did not encounter target node`)
