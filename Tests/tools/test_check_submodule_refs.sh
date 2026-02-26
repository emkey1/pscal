#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
CHECK_SCRIPT="${ROOT_DIR}/Tools/check_submodule_refs.sh"

TMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/check-submodule-refs-test.XXXXXX")"
trap 'rm -rf "$TMP_DIR"' EXIT

export GIT_ALLOW_PROTOCOL="file:https:http"

pass_count=0

log_pass() {
    echo "PASS: $1"
    pass_count=$((pass_count + 1))
}

run_expect_success() {
    local desc="$1"
    shift
    local out_file="${TMP_DIR}/out-success.log"
    if "$@" >"$out_file" 2>&1; then
        log_pass "$desc"
        return
    fi
    echo "FAIL: ${desc}" >&2
    cat "$out_file" >&2
    exit 1
}

run_expect_failure() {
    local desc="$1"
    local expected_pattern="$2"
    shift 2
    local out_file="${TMP_DIR}/out-fail.log"
    set +e
    "$@" >"$out_file" 2>&1
    local rc=$?
    set -e
    if [[ "$rc" -eq 0 ]]; then
        echo "FAIL: ${desc} (expected failure, got success)" >&2
        cat "$out_file" >&2
        exit 1
    fi
    if [[ "$rc" -ne 1 ]]; then
        echo "FAIL: ${desc} (expected exit 1, got ${rc})" >&2
        cat "$out_file" >&2
        exit 1
    fi
    if ! grep -q "$expected_pattern" "$out_file"; then
        echo "FAIL: ${desc} (missing pattern: ${expected_pattern})" >&2
        cat "$out_file" >&2
        exit 1
    fi
    log_pass "$desc"
}

init_repo() {
    local repo="$1"
    git -C "$repo" config user.name "submodule-check-test"
    git -C "$repo" config user.email "submodule-check-test@example.com"
}

SUB_WORK="${TMP_DIR}/sub-work"
SUB_REMOTE="${TMP_DIR}/sub-remote.git"
SUPER_WORK="${TMP_DIR}/super-work"
SUPER_REMOTE="${TMP_DIR}/super-remote.git"
SUPER_CLONE="${TMP_DIR}/super-clone"

git init -q "$SUB_WORK"
init_repo "$SUB_WORK"
echo "v1" > "${SUB_WORK}/payload.txt"
git -C "$SUB_WORK" add payload.txt
git -C "$SUB_WORK" commit -qm "submodule v1"
git -C "$SUB_WORK" branch -M main
git clone -q --bare "$SUB_WORK" "$SUB_REMOTE"
git -C "$SUB_WORK" remote add origin "$SUB_REMOTE"
git -C "$SUB_WORK" push -q origin main

git init -q "$SUPER_WORK"
init_repo "$SUPER_WORK"
git -C "$SUPER_WORK" -c protocol.file.allow=always submodule add "$SUB_REMOTE" deps/sub >/dev/null 2>&1
git -C "$SUPER_WORK" commit -qm "add submodule"
git -C "$SUPER_WORK" branch -M main

echo "v2" > "${SUB_WORK}/payload.txt"
git -C "$SUB_WORK" add payload.txt
git -C "$SUB_WORK" commit -qm "submodule v2"
git -C "$SUB_WORK" push -q origin main
NEW_SUB_SHA="$(git -C "$SUB_WORK" rev-parse HEAD)"

git -C "$SUPER_WORK" checkout -qb devel
git -C "$SUPER_WORK/deps/sub" fetch -q origin
git -C "$SUPER_WORK/deps/sub" checkout -q "$NEW_SUB_SHA"
git -C "$SUPER_WORK" add deps/sub
git -C "$SUPER_WORK" commit -qm "update submodule on devel"

git -C "$SUPER_WORK" checkout -qb broken
BAD_SHA="deadbeefdeadbeefdeadbeefdeadbeefdeadbeef"
git -C "$SUPER_WORK" update-index --cacheinfo "160000,${BAD_SHA},deps/sub"
git -C "$SUPER_WORK" commit -qm "introduce broken submodule pointer"

git clone -q --bare "$SUPER_WORK" "$SUPER_REMOTE"
git clone -q "$SUPER_REMOTE" "$SUPER_CLONE"

run_expect_success \
    "single-ref check succeeds for origin/main" \
    bash -lc "cd \"$SUPER_CLONE\" && \"$CHECK_SCRIPT\" --ref origin/main"

run_expect_success \
    "multi-ref check succeeds for origin/main + origin/devel" \
    bash -lc "cd \"$SUPER_CLONE\" && \"$CHECK_SCRIPT\" --ref origin/main --ref origin/devel"

run_expect_success \
    "protected-refs shorthand checks origin/main and origin/devel" \
    bash -lc "cd \"$SUPER_CLONE\" && \"$CHECK_SCRIPT\" --protected-refs --remote origin"

run_expect_failure \
    "broken submodule pointer is reported as a failure" \
    "remote did not provide pinned commit" \
    bash -lc "cd \"$SUPER_CLONE\" && \"$CHECK_SCRIPT\" --ref origin/broken"

echo "All submodule checker tests passed (${pass_count} assertions)."
