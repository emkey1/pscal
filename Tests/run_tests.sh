#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
PSCAL_BIN="$ROOT_DIR/build/bin/pscal"

if [ ! -x "$PSCAL_BIN" ]; then
  echo "pscal binary not found at $PSCAL_BIN" >&2
  exit 1
fi

CRT_UNIT="$ROOT_DIR/lib/crtvt.pl"
if [ -f "$CRT_UNIT" ]; then
  install -d /usr/local/Pscal/lib
  cp "$CRT_UNIT" /usr/local/Pscal/lib/crt.pl
fi

NEGATIVE_TESTS=(
  "ArgumentOrderMismatch.p"
  "ArgumentTypeMismatch.p"
  "ArrayArgumentMismatch.p"
  "OpenArrayBaseTypeMismatch.p"
)

if grep -q '^SDL:BOOL=ON$' "$ROOT_DIR/build/CMakeCache.txt" 2>/dev/null; then
  SDL_ENABLED=1
else
  SDL_ENABLED=0
fi

mapfile -t ALL_TESTS < <(find "$SCRIPT_DIR" -name '*.p' -not -path "$SCRIPT_DIR/Archived/*" -print | sort | sed "s|^$SCRIPT_DIR/||")

if [ "$SDL_ENABLED" -eq 0 ]; then
  ALL_TESTS=($(printf "%s\n" "${ALL_TESTS[@]}" | grep -v '^SDLFeaturesTest' | grep -v 'SDLRenderCopyTest.p'))
fi

POSITIVE_TESTS=()
for t in "${ALL_TESTS[@]}"; do
  skip=0
  for neg in "${NEGATIVE_TESTS[@]}"; do
    if [ "$t" = "$neg" ]; then
      skip=1
      break
    fi
  done
  if [ $skip -eq 0 ]; then
    POSITIVE_TESTS+=("$t")
  fi
done

EXIT_CODE=0

echo "Running positive tests..."
for t in "${POSITIVE_TESTS[@]}"; do
  echo "---- $t ----"
  if ! "$PSCAL_BIN" "$SCRIPT_DIR/$t"; then
    echo "Test failed: $t" >&2
    EXIT_CODE=1
  fi
  echo
  echo
done

echo "Running negative tests..."
for t in "${NEGATIVE_TESTS[@]}"; do
  if [[ ! " ${ALL_TESTS[*]} " == *" $t "* ]]; then
    continue
  fi
  echo "---- $t (expected failure) ----"
  if "$PSCAL_BIN" "$SCRIPT_DIR/$t" >/tmp/pscal_output 2>&1; then
    echo "ERROR: $t unexpectedly succeeded" >&2
    cat /tmp/pscal_output
    EXIT_CODE=1
  else
    cat /tmp/pscal_output
    if [ -f "$SCRIPT_DIR/${t%.p}.dbg" ]; then
      echo "ERROR: bytecode generated for $t" >&2
      rm -f "$SCRIPT_DIR/${t%.p}.dbg"
      EXIT_CODE=1
    fi
  fi
  echo
  echo
done

exit $EXIT_CODE
