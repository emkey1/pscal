#!/usr/bin/env bash
set -euo pipefail

# Deterministic fixture for SmallClue git Phase A parity tests.
# Prints shell-style key/value lines for harness token expansion.

if [[ "${1:-}" != "" ]]; then
  FIXTURE_ROOT="$1"
  rm -rf "$FIXTURE_ROOT"
  mkdir -p "$FIXTURE_ROOT"
else
  tmp_base="${TMPDIR:-/tmp}"
  tmp_base="${tmp_base%/}"
  FIXTURE_ROOT="$(mktemp -d "${tmp_base}/pscal_git_phase_a_fixture.XXXXXX")"
fi

repo="$FIXTURE_ROOT/repo"
mkdir -p "$repo"
cd "$repo"
repo="$(pwd -P)"

git init -b main >/dev/null 2>&1
git config user.name "PSCAL Tester"
git config user.email "pscal@example.com"

cat > README.md <<'EOF'
line1
line2
EOF
GIT_AUTHOR_DATE='2024-01-01T00:00:00Z' GIT_COMMITTER_DATE='2024-01-01T00:00:00Z' git add README.md
GIT_AUTHOR_DATE='2024-01-01T00:00:00Z' GIT_COMMITTER_DATE='2024-01-01T00:00:00Z' git commit -m 'initial commit' >/dev/null

cat > notes.txt <<'EOF'
alpha
beta
EOF
GIT_AUTHOR_DATE='2024-01-02T00:00:00Z' GIT_COMMITTER_DATE='2024-01-02T00:00:00Z' git add notes.txt
GIT_AUTHOR_DATE='2024-01-02T00:00:00Z' GIT_COMMITTER_DATE='2024-01-02T00:00:00Z' git commit -m 'add notes' >/dev/null

GIT_COMMITTER_DATE='2024-01-02T12:00:00Z' git tag -a v1.0 -m 'release 1.0'

git checkout -b feature >/dev/null 2>&1
cat > feature.txt <<'EOF'
feature branch
EOF
GIT_AUTHOR_DATE='2024-01-03T00:00:00Z' GIT_COMMITTER_DATE='2024-01-03T00:00:00Z' git add feature.txt
GIT_AUTHOR_DATE='2024-01-03T00:00:00Z' GIT_COMMITTER_DATE='2024-01-03T00:00:00Z' git commit -m 'feature work' >/dev/null

git checkout main >/dev/null 2>&1
printf 'gamma\n' >> notes.txt
GIT_AUTHOR_DATE='2024-01-04T00:00:00Z' GIT_COMMITTER_DATE='2024-01-04T00:00:00Z' git add notes.txt
GIT_AUTHOR_DATE='2024-01-04T00:00:00Z' GIT_COMMITTER_DATE='2024-01-04T00:00:00Z' git commit -m 'update notes' >/dev/null

# Dirty working tree state for status/diff coverage.
printf 'worktree change\n' >> README.md
cat > staged.txt <<'EOF'
staged line
EOF
git add staged.txt
cat > untracked.log <<'EOF'
untracked line
EOF

head_oid="$(git rev-parse HEAD)"
head_short="$(git rev-parse --short HEAD)"
feature_short12="$(git rev-parse --short=12 feature)"
top_level="$(git rev-parse --show-toplevel)"
tag_target="$(git rev-list -n 1 v1.0)"

printf 'FIXTURE_ROOT=%s\n' "$FIXTURE_ROOT"
printf 'REPO_ROOT=%s\n' "$repo"
printf 'HEAD_OID=%s\n' "$head_oid"
printf 'HEAD_SHORT=%s\n' "$head_short"
printf 'FEATURE_SHORT12=%s\n' "$feature_short12"
printf 'TOP_LEVEL=%s\n' "$top_level"
printf 'TAG_TARGET=%s\n' "$tag_target"
