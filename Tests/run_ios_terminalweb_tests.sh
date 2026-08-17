#!/bin/bash
set -euo pipefail

# Guards on the bundled hterm terminal assets used by the iOS/iPadOS app.
#
# hterm_all.js is vendored, and the scrollback cap in it is a local patch: the
# upstream file retains every row that scrolls off the top as a detached <x-row>
# DOM node, which grew the web content process until iOS killed the app.  These
# checks are wiring guards, not behavioural tests -- they exist so re-vendoring
# hterm can't silently drop the patch.  Behaviour has to be exercised in a real
# WebKit (see Docs/ios_terminal_scrollback.md).

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WEB_DIR="${SCRIPT_DIR}/../ios/Sources/App/TerminalWeb"
HTERM="${WEB_DIR}/hterm_all.js"
TERM_JS="${WEB_DIR}/term.js"

failures=0
fail() {
    printf 'FAIL: %s\n' "$1" >&2
    failures=$((failures + 1))
}
pass() {
    printf 'ok: %s\n' "$1"
}

for f in "${HTERM}" "${TERM_JS}"; do
    if [ ! -f "${f}" ]; then
        fail "missing terminal asset ${f}"
    fi
done
if [ "${failures}" -ne 0 ]; then
    exit 1
fi

# 1. Both files must parse.  node is optional; skip rather than fail without it.
if command -v node >/dev/null 2>&1; then
    for f in "${HTERM}" "${TERM_JS}"; do
        if node --check "${f}" >/dev/null 2>&1; then
            pass "$(basename "${f}") parses"
        else
            fail "$(basename "${f}") does not parse"
        fi
    done
else
    printf 'skip: node not available, not syntax checking the terminal assets\n'
fi

# 2. Every row that leaves the screen must go through pushScrollbackRows_, which
#    applies the cap and drops alternate-screen rows.  The only raw push allowed
#    is the one inside pushScrollbackRows_; any other means unbounded growth is
#    back at one of the three scroll-off sites.
raw_pushes=$(grep -cE 'scrollbackRows_\)?\.push|push\.apply\(this\.scrollbackRows_' "${HTERM}" || true)
if [ "${raw_pushes}" -eq 1 ] &&
   grep -qF 'Array.prototype.push.apply(this.scrollbackRows_, rows);' "${HTERM}"; then
    pass "the only raw scrollbackRows_ push is the one inside pushScrollbackRows_"
else
    fail "found ${raw_pushes} raw scrollbackRows_ push(es); expected exactly the one inside pushScrollbackRows_"
fi

push_sites=$(grep -c 'this\.pushScrollbackRows_(' "${HTERM}" || true)
if [ "${push_sites}" -eq 3 ]; then
    pass "all 3 scroll-off sites route through pushScrollbackRows_"
else
    fail "expected 3 pushScrollbackRows_ call sites in hterm_all.js, found ${push_sites}"
fi

# 3. The cap machinery itself.
for symbol in 'hterm.Terminal.DEFAULT_SCROLLBACK_LIMIT' \
              'hterm.Terminal.prototype.setScrollbackLimit' \
              'hterm.Terminal.prototype.pushScrollbackRows_' \
              'hterm.Terminal.prototype.trimScrollback_'; do
    if grep -qF "${symbol}" "${HTERM}"; then
        pass "hterm_all.js defines ${symbol}"
    else
        fail "hterm_all.js is missing ${symbol}"
    fi
done

# 4. Alternate-screen rows must not be retained: full-screen apps repaint
#    continuously, so keeping their rows grows memory for as long as they run.
if grep -qF 'this.screen_ === this.alternateScreen_' "${HTERM}"; then
    pass "pushScrollbackRows_ guards against the alternate screen"
else
    fail "hterm_all.js lost the alternate-screen guard in pushScrollbackRows_"
fi

# 5. term.js has to actually apply a limit, or the default is all that stands
#    between a long session and the jetsam limit.
if grep -qE 'term\.setScrollbackLimit\(' "${TERM_JS}"; then
    pass "term.js applies a scrollback limit at startup"
else
    fail "term.js no longer calls term.setScrollbackLimit()"
fi

if [ "${failures}" -ne 0 ]; then
    printf '\n%d terminal-web check(s) failed\n' "${failures}" >&2
    exit 1
fi

printf '\nAll terminal-web checks passed\n'
