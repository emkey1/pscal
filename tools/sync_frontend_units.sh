#!/usr/bin/env bash
# lib/<language> here is the source of the library units. Each frontend repo
# keeps a copy in lib/<language>, so a clone of it can compile its examples.
#
#   tools/sync_frontend_units.sh           copy lib/<language> into components/<language>
#   tools/sync_frontend_units.sh --check   fail if a copy differs (CI runs this)
set -uo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
check=0
case "${1:-}" in
    "") ;;
    --check) check=1 ;;
    *) echo "usage: $0 [--check]" >&2; exit 2 ;;
esac

status=0
for language in pascal rea clike; do
    source_dir="$root/lib/$language"
    copy_dir="$root/components/$language/lib/$language"
    if [ "$check" -eq 1 ]; then
        if ! diff -r "$source_dir" "$copy_dir"; then
            echo "components/$language/lib/$language differs from lib/$language; run tools/sync_frontend_units.sh and commit in $language" >&2
            status=1
        fi
    else
        mkdir -p "$copy_dir" && rm -rf "$copy_dir" && cp -R "$source_dir" "$copy_dir" || status=1
    fi
done
exit "$status"
