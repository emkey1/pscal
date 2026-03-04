# iOS SSH Race Retest (Debug Build)

Use this checklist when validating SSH tab launch stability on iOS/iPadOS.

## 1. Enable runtime debug logs

In Xcode scheme environment variables for `PscalApp` Debug:

- `PSCALI_SSH_DEBUG=1`
- `PSCALI_TOOL_DEBUG=1`

## 2. Attach debugger and launch app

- Run from Xcode with debugger attached.
- Open a normal shell tab in the app.

## 3. Stress SSH tab creation

Run an SSH command repeatedly from the shell tab (replace host/user):

```sh
for i in $(seq 1 40); do
  echo "[ssh-race-probe] iteration $i"
  ssh -o BatchMode=yes -o ConnectTimeout=5 user@host "echo ok" || true
done
```

Also repeat manual launches quickly:

- Open several SSH tabs in succession.
- Switch tabs while connections are being established.

## 4. Watch Xcode console logs

Look for `[ssh-session]` logs and check for failures:

- `banner exchange: Connection to UNKNOWN port -1: Socket is not connected`
- unexpected `SSH exited with status 255` during healthy network conditions

You can run the analyzer on an exported Xcode log:

```sh
python3 Tests/ios_vproc/ssh_debug_log_analyzer.py /path/to/xcode.log
```

For CI/automation style gating:

```sh
python3 Tests/ios_vproc/ssh_debug_log_analyzer.py --fail-on-warning /path/to/xcode.log
```

## 5. Pass criteria

- No intermittent `UNKNOWN port -1` failures during repeated runs.
- Session creation/teardown logs remain paired (create/start/exit per session).
- Behavior remains stable both with and without debugger attached.
