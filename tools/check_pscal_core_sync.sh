#!/usr/bin/env bash
# Cheap, read-only check that PBuild's two independent pscal-core pins agree:
#   1. the top-level components/pscal-core submodule
#   2. components/aether's nested external/pscal-core pin
# These are genuinely independent submodule pointers (confirmed 2026-07-22)
# and nothing keeps them in sync automatically -- they were found already
# diverged purely from ordinary work, with no mistake involved. This script
# just detects that drift loudly and immediately, instead of it surfacing
# later as a confusing build/behavior mismatch. To fix drift, run
# tools/bump_pscal_core.sh.
#
# Reads both pins straight from git's object db via `git ls-tree` (same
# pattern tools/deploy_aether_to_claws.sh uses for the aether gitlink) --
# does not require either submodule to be checked out at HEAD, only that
# their commits are present locally (i.e. `git submodule update --init`).
#
# Usage: tools/check_pscal_core_sync.sh [--ref <git-ref>]
#   --ref   PBuild ref to check (default: HEAD)
#
# Exit codes: 0 = in sync, 1 = drifted, 2 = could not check (e.g. submodules
# not initialized -- not treated as drift).
#
# Wire this in as a pre-commit hook or CI step to catch drift immediately;
# safe to run by hand any time too.
set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REF="HEAD"

while [ $# -gt 0 ]; do
  case "$1" in
    --ref) shift; REF="${1:-}";;
    -h|--help)
      sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'
      exit 0
      ;;
    *) echo "error: unknown argument: $1" >&2; exit 2;;
  esac
  shift
done

cd "$ROOT" || exit 2

AETHER_SHA="$(git ls-tree "$REF" -- components/aether 2>/dev/null | awk '{print $3}')"
TOP_SHA="$(git ls-tree "$REF" -- components/pscal-core 2>/dev/null | awk '{print $3}')"

if [ -z "$TOP_SHA" ] || [ -z "$AETHER_SHA" ]; then
  echo "check_pscal_core_sync: could not read one or both gitlinks at ${REF} (top=${TOP_SHA:-?} aether=${AETHER_SHA:-?})" >&2
  exit 2
fi

if ! git -C components/aether cat-file -e "${AETHER_SHA}^{commit}" 2>/dev/null; then
  echo "check_pscal_core_sync: components/aether commit ${AETHER_SHA} not present locally -- run 'git submodule update --init components/aether' first" >&2
  exit 2
fi

NESTED_SHA="$(git -C components/aether ls-tree "$AETHER_SHA" -- external/pscal-core 2>/dev/null | awk '{print $3}')"
if [ -z "$NESTED_SHA" ]; then
  echo "check_pscal_core_sync: components/aether @ ${AETHER_SHA:0:7} has no external/pscal-core entry (unexpected)" >&2
  exit 2
fi

if [ "$TOP_SHA" = "$NESTED_SHA" ]; then
  echo "check_pscal_core_sync: OK -- pscal-core in sync @ ${TOP_SHA:0:7} (top-level and components/aether@${AETHER_SHA:0:7}/external/pscal-core agree)"
  exit 0
fi

cat >&2 <<EOF
check_pscal_core_sync: DRIFT -- pscal-core pins disagree at ${REF}
  components/pscal-core (top-level):                   ${TOP_SHA}
  components/aether@${AETHER_SHA:0:7}/external/pscal-core: ${NESTED_SHA}
Fix with: tools/bump_pscal_core.sh
EOF
exit 1
