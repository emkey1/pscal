#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<'EOF'
Usage: Tools/check_submodule_refs.sh [--ref <git-ref>] [--offline]

Checks that submodule commits pinned by <git-ref> are fetchable.

Modes:
  default    online check via "git fetch <url> <sha>" in a temp repo
  --offline  verify pinned sha exists locally and is contained in at least one
             locally-known remote-tracking branch
EOF
}

ref="HEAD"
offline=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --ref)
            shift
            if [[ $# -eq 0 ]]; then
                echo "error: --ref requires a value" >&2
                exit 2
            fi
            ref="$1"
            ;;
        --offline)
            offline=1
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "error: unknown argument: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
    shift
done

repo_root="$(git rev-parse --show-toplevel 2>/dev/null || true)"
if [[ -z "$repo_root" ]]; then
    echo "error: not inside a git repository" >&2
    exit 2
fi
cd "$repo_root"

if [[ ! -f .gitmodules ]]; then
    echo "No .gitmodules file found; nothing to check."
    exit 0
fi

if ! git rev-parse --verify "${ref}^{commit}" >/dev/null 2>&1; then
    echo "error: ref '${ref}' is not a valid commit" >&2
    exit 2
fi

failures=0
checked=0

while read -r key path; do
    name="${key#submodule.}"
    name="${name%.path}"
    url="$(git config -f .gitmodules --get "submodule.${name}.url" || true)"
    if [[ -z "$url" ]]; then
        echo "FAIL: ${path}: missing URL in .gitmodules"
        failures=$((failures + 1))
        continue
    fi

    pinned_sha="$(git ls-tree "$ref" -- "$path" | awk '{print $3}')"
    if [[ -z "$pinned_sha" ]]; then
        echo "SKIP: ${path}: not present at ${ref}"
        continue
    fi

    checked=$((checked + 1))
    printf 'Checking %s @ %s\n' "$path" "$pinned_sha"

    if [[ "$offline" -eq 1 ]]; then
        if [[ ! -d "$path/.git" && ! -f "$path/.git" ]]; then
            echo "FAIL: ${path}: submodule not initialized (required for --offline)"
            failures=$((failures + 1))
            continue
        fi
        if ! git -C "$path" cat-file -e "${pinned_sha}^{commit}" >/dev/null 2>&1; then
            echo "FAIL: ${path}: pinned commit is missing locally"
            failures=$((failures + 1))
            continue
        fi
        if ! git -C "$path" branch -r --contains "$pinned_sha" | grep -q '[^[:space:]]'; then
            echo "FAIL: ${path}: commit not contained in any locally-known remote branch"
            failures=$((failures + 1))
            continue
        fi
        echo "OK:   ${path}: present locally and reachable from remote-tracking refs"
    else
        tmp_repo="$(mktemp -d "${TMPDIR:-/tmp}/pscal-submodule-check.XXXXXX")"
        if ! git -C "$tmp_repo" init -q >/dev/null 2>&1; then
            echo "FAIL: ${path}: unable to initialize temp repo"
            rm -rf "$tmp_repo"
            failures=$((failures + 1))
            continue
        fi
        if ! git -C "$tmp_repo" fetch --quiet --depth=1 "$url" "$pinned_sha" >/dev/null 2>&1; then
            echo "FAIL: ${path}: remote did not provide pinned commit"
            rm -rf "$tmp_repo"
            failures=$((failures + 1))
            continue
        fi
        rm -rf "$tmp_repo"
        echo "OK:   ${path}: remote serves pinned commit"
    fi
done < <(git config -f .gitmodules --get-regexp '^submodule\..*\.path$' || true)

if [[ "$checked" -eq 0 ]]; then
    echo "No submodule paths found in .gitmodules."
    exit 0
fi

if [[ "$failures" -ne 0 ]]; then
    echo "Submodule reference check failed: ${failures} issue(s)."
    exit 1
fi

echo "Submodule reference check passed (${checked} submodule(s))."
