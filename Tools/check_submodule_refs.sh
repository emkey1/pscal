#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<'EOF'
Usage: Tools/check_submodule_refs.sh [--ref <git-ref>] [--protected-refs] [--remote <name>] [--offline]

Checks that submodule commits pinned by one or more refs are fetchable.

Modes:
  default    online check via "git fetch <url> <sha>" in a temp repo
  --offline  verify pinned sha exists locally and is contained in at least one
             locally-known remote-tracking branch

Ref selection:
  --ref <git-ref>     may be provided multiple times (default: HEAD)
  --protected-refs    shorthand for --ref <remote>/main and --ref <remote>/devel
  --remote <name>     remote name used by --protected-refs (default: origin)
EOF
}

refs=()
remote_name="origin"
offline=0
protected_refs=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --ref)
            shift
            if [[ $# -eq 0 ]]; then
                echo "error: --ref requires a value" >&2
                exit 2
            fi
            refs+=("$1")
            ;;
        --protected-refs)
            protected_refs=1
            ;;
        --remote)
            shift
            if [[ $# -eq 0 ]]; then
                echo "error: --remote requires a value" >&2
                exit 2
            fi
            remote_name="$1"
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

if [[ "$protected_refs" -eq 1 ]]; then
    refs+=("${remote_name}/main" "${remote_name}/devel")
fi

if [[ "${#refs[@]}" -eq 0 ]]; then
    refs=("HEAD")
fi

# Keep first-seen order while removing duplicates.
unique_refs=()
for ref in "${refs[@]}"; do
    already=0
    for seen in "${unique_refs[@]-}"; do
        if [[ "$seen" == "$ref" ]]; then
            already=1
            break
        fi
    done
    if [[ "$already" -eq 0 ]]; then
        unique_refs+=("$ref")
    fi
done
refs=("${unique_refs[@]}")

for ref in "${refs[@]}"; do
    if ! git rev-parse --verify "${ref}^{commit}" >/dev/null 2>&1; then
        echo "error: ref '${ref}' is not a valid commit" >&2
        exit 2
    fi
done

submodule_entries=()
while IFS= read -r entry; do
    submodule_entries+=("$entry")
done < <(git config -f .gitmodules --get-regexp '^submodule\..*\.path$' || true)
if [[ "${#submodule_entries[@]}" -eq 0 ]]; then
    echo "No submodule paths found in .gitmodules."
    exit 0
fi

failures=0
checked=0
refs_checked=0

for ref in "${refs[@]}"; do
    refs_checked=$((refs_checked + 1))
    ref_failures=0
    ref_checked=0
    echo "== Ref: ${ref} =="

    for entry in "${submodule_entries[@]}"; do
        key="${entry%% *}"
        path="${entry#* }"
        name="${key#submodule.}"
        name="${name%.path}"
        url="$(git config -f .gitmodules --get "submodule.${name}.url" || true)"
        if [[ -z "$url" ]]; then
            echo "FAIL: [${ref}] ${path}: missing URL in .gitmodules"
            failures=$((failures + 1))
            ref_failures=$((ref_failures + 1))
            continue
        fi

        pinned_sha="$(git ls-tree "$ref" -- "$path" | awk '{print $3}')"
        if [[ -z "$pinned_sha" ]]; then
            echo "SKIP: [${ref}] ${path}: not present at ${ref}"
            continue
        fi

        checked=$((checked + 1))
        ref_checked=$((ref_checked + 1))
        printf 'Checking [%s] %s @ %s\n' "$ref" "$path" "$pinned_sha"

        if [[ "$offline" -eq 1 ]]; then
            if [[ ! -d "$path/.git" && ! -f "$path/.git" ]]; then
                echo "FAIL: [${ref}] ${path}: submodule not initialized (required for --offline)"
                failures=$((failures + 1))
                ref_failures=$((ref_failures + 1))
                continue
            fi
            if ! git -C "$path" cat-file -e "${pinned_sha}^{commit}" >/dev/null 2>&1; then
                echo "FAIL: [${ref}] ${path}: pinned commit is missing locally"
                failures=$((failures + 1))
                ref_failures=$((ref_failures + 1))
                continue
            fi
            if ! git -C "$path" branch -r --contains "$pinned_sha" | grep -q '[^[:space:]]'; then
                echo "FAIL: [${ref}] ${path}: commit not contained in any locally-known remote branch"
                failures=$((failures + 1))
                ref_failures=$((ref_failures + 1))
                continue
            fi
            echo "OK:   [${ref}] ${path}: present locally and reachable from remote-tracking refs"
            continue
        fi

        tmp_repo="$(mktemp -d "${TMPDIR:-/tmp}/pscal-submodule-check.XXXXXX")"
        fetch_log="${tmp_repo}/fetch.log"
        if ! git -C "$tmp_repo" init -q >/dev/null 2>&1; then
            echo "FAIL: [${ref}] ${path}: unable to initialize temp repo"
            rm -rf "$tmp_repo"
            failures=$((failures + 1))
            ref_failures=$((ref_failures + 1))
            continue
        fi
        if ! git -C "$tmp_repo" fetch --quiet --depth=1 "$url" "$pinned_sha" >/dev/null 2>"$fetch_log"; then
            echo "FAIL: [${ref}] ${path}: remote did not provide pinned commit (${pinned_sha})"
            echo "      remote: ${url}"
            if [[ -s "$fetch_log" ]]; then
                while IFS= read -r line; do
                    echo "      ${line}"
                done < <(tail -n 2 "$fetch_log")
            fi
            rm -rf "$tmp_repo"
            failures=$((failures + 1))
            ref_failures=$((ref_failures + 1))
            continue
        fi
        rm -rf "$tmp_repo"
        echo "OK:   [${ref}] ${path}: remote serves pinned commit"
    done

    if [[ "$ref_checked" -eq 0 ]]; then
        echo "Ref ${ref}: no submodule paths were present."
    else
        echo "Ref ${ref}: checked ${ref_checked} submodule(s), ${ref_failures} issue(s)."
    fi
done

if [[ "$checked" -eq 0 || "$refs_checked" -eq 0 ]]; then
    echo "No submodule references were checked."
    exit 0
fi

if [[ "$failures" -ne 0 ]]; then
    echo "Submodule reference check failed: ${failures} issue(s) across ${refs_checked} ref(s)."
    exit 1
fi

echo "Submodule reference check passed (${checked} submodule(s) across ${refs_checked} ref(s))."
