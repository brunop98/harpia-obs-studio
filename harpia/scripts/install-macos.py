#!/usr/bin/env python3
"""Install a verified local Harpia build without requiring administrator access."""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import plistlib
import subprocess
import sys
import tempfile

BUNDLE_ID = "com.harpia.recorder"


def run(command: list[str]) -> str:
    result = subprocess.run(command, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if result.returncode:
        raise RuntimeError(f"Command failed: {' '.join(command)}\n{result.stdout}")
    return result.stdout


def validate(app: Path) -> None:
    if app.is_symlink():
        raise RuntimeError(f"Refusing to replace an application symlink: {app}")
    with (app / "Contents/Info.plist").open("rb") as file:
        info = plistlib.load(file)
    if info.get("CFBundleIdentifier") != BUNDLE_ID or info.get("CFBundleExecutable") != "Harpia":
        raise RuntimeError(f"Not a Harpia application: {app}")
    if not (app / "Contents/MacOS/Harpia").is_file():
        raise RuntimeError(f"Missing Harpia executable: {app}")


def default_destination() -> Path:
    system = Path("/Applications")
    return system if system.is_dir() and os.access(system, os.W_OK) else Path.home() / "Applications"


def install(app: Path, destination: Path) -> Path:
    validate(app)
    # Updating a running bundle can leave the old process handling launches
    # with stale permissions. Never quit it automatically or discard edits.
    processes = run(["/bin/ps", "-axo", "comm="])
    if any(line.strip().endswith("/Contents/MacOS/Harpia") for line in processes.splitlines()):
        raise RuntimeError("Quit Harpia (including editor windows) before installing, then run the command again.")
    run(["/usr/bin/codesign", "--verify", "--deep", "--strict", str(app)])
    destination = destination.expanduser().resolve()
    target = destination / "Harpia.app"
    if target == app.resolve():
        return target
    if target.exists() or target.is_symlink():
        validate(target)  # Never replace another app or an unexpected directory.
    destination.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".harpia-install-", dir=destination) as scratch:
        stage = Path(scratch) / "Harpia.app"
        backup = Path(scratch) / "previous.app"
        run(["/usr/bin/ditto", str(app), str(stage)])
        validate(stage)
        run(["/usr/bin/codesign", "--verify", "--deep", "--strict", str(stage)])
        had_previous = target.exists()
        if had_previous:
            target.rename(backup)
        try:
            stage.rename(target)
        except OSError:
            if had_previous:
                backup.rename(target)
            raise
    return target


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--app", type=Path, required=True)
    parser.add_argument("--destination", type=Path)
    args = parser.parse_args()
    if sys.platform != "darwin":
        parser.error("Installing Harpia.app requires macOS")
    try:
        print(install(args.app, args.destination or default_destination()))
        return 0
    except (OSError, RuntimeError, ValueError, plistlib.InvalidFileException) as error:
        print(f"Harpia installation failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
