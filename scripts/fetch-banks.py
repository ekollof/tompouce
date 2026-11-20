#!/usr/bin/env python3
"""Fetch the full pinned sound-bank set using the project's own installer.

Used by the AppImage on first run. The lock files and install.py must sit in
the same scripts/ directory next to this helper (the AppImage bundles them).
The download is resumable: the installer's receipt verifies every file it has
already written. A lock prevents two AppImage instances from fetching at once.
"""
import fcntl
import importlib.util
import json
import sys
from pathlib import Path

if len(sys.argv) != 2:
    raise SystemExit("usage: fetch-banks.py <banks-root>")

scripts = Path(__file__).resolve().parent
installer_path = scripts / "install.py"
if not installer_path.is_file():
    raise SystemExit(f"installer not found: {installer_path}")
spec = importlib.util.spec_from_file_location("tompouce_bank_installer", installer_path)
installer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(installer)

root = Path(sys.argv[1]).expanduser().resolve()
root.mkdir(parents=True, exist_ok=True)
lock_path = root.parent / ".banks-fetch.lock"
lock_path.parent.mkdir(parents=True, exist_ok=True)
with lock_path.open("w") as lock:
    fcntl.flock(lock, fcntl.LOCK_EX)
    if (root / ".install-receipt.json").is_file():
        print("sound banks already installed", flush=True)
        raise SystemExit(0)
    lock = json.loads((scripts / "banks.lock.json").read_text())
    installer.BankInstaller(root, 8).install(lock, True)
    installer.install_gm_webaudiofont(root, 8, True)
