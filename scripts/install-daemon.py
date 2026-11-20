#!/usr/bin/env python3
"""Install an isolated Tompouce daemon as a systemd user service (disabled).

The instance owns its token and socket under $XDG_RUNTIME_DIR/tompouce (or a
chosen directory). It never touches an interactive session's source, playback
or config. The service is written but NOT enabled: start it explicitly with
``systemctl --user start tompouce-daemon``.
"""
import argparse
import os
import secrets
import sys
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--prefix", type=Path, help="installed binary prefix (default: repo build dir)")
    parser.add_argument("--unit-dir", type=Path, help="systemd user unit directory")
    parser.add_argument("--runtime-dir", type=Path, help="daemon runtime directory")
    parser.add_argument("--force", action="store_true", help="replace an existing unit/token")
    args = parser.parse_args()

    source = Path(__file__).resolve().parent.parent
    if args.prefix:
        binary = args.prefix / "bin" / "tompouce"
    else:
        binary = source / "build" / "tompouce"
    if not binary.is_file():
        print(f"install-daemon: binary not found: {binary}", file=sys.stderr)
        return 1

    unit_dir = args.unit_dir or (Path(os.environ.get("XDG_CONFIG_HOME", Path.home() / ".config")) / "systemd" / "user")
    runtime = args.runtime_dir or (Path(os.environ.get("XDG_RUNTIME_DIR", Path.home() / ".local" / "state" / "tompouce")) / "tompouce")
    token_path = runtime / "token"

    runtime.mkdir(parents=True, exist_ok=True, mode=0o700)
    if token_path.exists() and not args.force:
        print(f"install-daemon: token exists ({token_path}); use --force to replace", file=sys.stderr)
        return 1
    os.chmod(runtime, 0o700)
    token_path.write_text(secrets.token_hex(32) + "\n")
    os.chmod(token_path, 0o600)

    template = (source / "systemd" / "tompouce-daemon.service.in").read_text()
    unit = template.replace("@BIN_DIR@", str(binary.parent)).replace("@DAEMON_DIR@", str(runtime))
    unit_dir.mkdir(parents=True, exist_ok=True)
    unit_path = unit_dir / "tompouce-daemon.service"
    if unit_path.exists() and not args.force:
        print(f"install-daemon: unit exists ({unit_path}); use --force to replace", file=sys.stderr)
        return 1
    unit_path.write_text(unit)

    print(f"installed {unit_path}")
    print(f"token {token_path} (0600), socket {runtime / 'mcp.sock'}")
    print("service is disabled; start with: systemctl --user start tompouce-daemon")
    print("then: python3 scripts/tompoucectl.py --name music_get_status")
    return 0


if __name__ == "__main__":
    sys.exit(main())
