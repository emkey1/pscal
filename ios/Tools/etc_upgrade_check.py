#!/usr/bin/env python3
"""An app update keeps the owner's /etc edits, and updates what is still as shipped.

Installs the app on a simulator, launches it once so it installs its etc, then makes the
container look like an earlier build left it: the version marker is old, one shipped file
holds an earlier build's text (recorded as shipped), and the owner has edited others. A second
launch is the update. Then:

- the owner's edits to hosts (an edited file) and words (any shipped file) are still there,
- termcap, unchanged since the earlier build shipped it, is the new build's,
- fstab, removed, is back.

It runs that twice: with the record of what was shipped, and with none, as the first update to
this scheme finds the container. Without a record only the files people edit are kept.

Usage: etc_upgrade_check.py --app PATH --device UDID
"""

import argparse
import hashlib
import json
import plistlib
import subprocess
import sys
import time
from pathlib import Path

BUNDLE_ID = "com.pscal.PSCAL"


def simctl(*args: str, check: bool = True) -> str:
    out = subprocess.run(["xcrun", "simctl", *args], capture_output=True, text=True, timeout=300)
    if check and out.returncode != 0:
        raise SystemExit(f"simctl {' '.join(args)} failed: {out.stderr.strip()}")
    return out.stdout.strip()


def launch_until_installed(udid: str, docs: Path, version: str) -> None:
    simctl("launch", udid, BUNDLE_ID)
    marker = docs / ".etc.version"
    for _ in range(120):
        if marker.exists() and marker.read_text().strip() == version:
            break
        time.sleep(0.5)
    else:
        raise SystemExit("the app never wrote its etc version marker")
    time.sleep(2)
    simctl("terminate", udid, BUNDLE_ID, check=False)
    time.sleep(1)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--app", required=True)
    ap.add_argument("--device", required=True)
    args = ap.parse_args()
    app = Path(args.app)
    udid = args.device
    version = plistlib.loads((app / "Info.plist").read_bytes())["CFBundleVersion"]
    bundle_etc = app / "etc"

    simctl("terminate", udid, BUNDLE_ID, check=False)
    simctl("uninstall", udid, BUNDLE_ID, check=False)
    time.sleep(3)
    simctl("install", udid, str(app))
    docs = Path(simctl("get_app_container", udid, BUNDLE_ID, "data")) / "Documents"
    launch_until_installed(udid, docs, version)
    etc = docs / "etc"
    failures = []

    for with_record in (True, False):
        label = "with a record" if with_record else "with no record"
        old_termcap = b"# an earlier build's termcap\n"
        hosts_line = "10.9.9.9 upgrade-kept\n"
        with open(etc / "hosts", "a") as f:
            f.write(hosts_line)
        (etc / "words").write_text("the owner's words\n")
        (etc / "termcap").write_bytes(old_termcap)
        (etc / "fstab").unlink()
        manifest = docs / ".etc.shipped"
        if with_record and manifest.exists():
            shipped = json.loads(manifest.read_text())
            shipped["termcap"] = hashlib.sha256(old_termcap).hexdigest()
            manifest.write_text(json.dumps(shipped))
        elif manifest.exists():
            manifest.unlink()
        (docs / ".etc.version").write_text("0\n")

        launch_until_installed(udid, docs, version)

        hosts = (etc / "hosts").read_text()
        if hosts_line not in hosts:
            failures.append(f"{label}: the owner's hosts line is gone")
        words = (etc / "words").read_text()
        want_words = "the owner's words\n" if with_record else (bundle_etc / "words").read_text()
        if words != want_words:
            failures.append(f"{label}: etc/words is {words[:40]!r}, want {want_words[:40]!r}")
        if (etc / "termcap").read_bytes() != (bundle_etc / "termcap").read_bytes():
            failures.append(f"{label}: termcap, as an earlier build shipped it, was not updated")
        if not (etc / "fstab").exists() or (etc / "fstab").read_bytes() != (bundle_etc / "fstab").read_bytes():
            failures.append(f"{label}: a removed fstab did not come back")
        # Put this round's edits back, so the next starts from the new build's etc.
        (etc / "hosts").write_bytes((bundle_etc / "hosts").read_bytes())
        (etc / "words").write_bytes((bundle_etc / "words").read_bytes())

    simctl("terminate", udid, BUNDLE_ID, check=False)
    for f in failures:
        print("FAIL", f)
    print("etc upgrade check:", "passed" if not failures else f"{len(failures)} failed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
