# SmallClue Git Parity Manifest

This directory defines the command-by-command golden parity matrix for the
current `git` SmallClue applet scope.

## Files

- `setup_phase_a_fixture.sh`: Creates a deterministic local git repository in a
  temp directory and prints key/value metadata for harnesses.
- `manifest.json`: Golden expectations for the currently-supported commands.
- `refresh_manifest.py`: Regenerates `manifest.json` from current system git
  output using the deterministic fixture.

## Covered Command Families

- `rev-parse`
- `config --get`
- `symbolic-ref`
- `rev-list`
- `show-ref`
- `ls-files`
- `status`
- `log`
- `show`
- `diff`
- `branch --list`
- `tag --list`

## Manifest Model

Each test entry includes:

- `git_argv`: argv passed to system git baseline (`git <git_argv...>`).
- `smallclue_argv`: argv passed to SmallClue (`smallclue <smallclue_argv...>`).
- `expected_exit`: exact expected exit code.
- `expected_stdout`: exact expected stdout.
- `expected_stderr`: exact expected stderr.
- `comparison`: currently all `exact` for `stdout`, `stderr`, and `exit`.

`expected_*` streams may contain `${REPO_ROOT}` tokens. Harnesses should replace
this token with the fixture repo path reported by `setup_phase_a_fixture.sh`
before comparison.

## Regenerate Goldens

From repo root:

```bash
python3 Tests/smallclue/git_phase_a/refresh_manifest.py
```
