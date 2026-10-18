#!/usr/bin/env bash
set -euo pipefail
install_source=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
if ! command -v python3 >/dev/null 2>&1; then
    for install_arg in "$@"; do
        if [[ $install_arg == --skip-deps ]]; then
            printf 'Python 3.10 or newer is required when using --skip-deps.\n' >&2
            exit 1
        fi
        if [[ $install_arg == --dry-run || $install_arg == --help || $install_arg == -h ]]; then
            printf 'Python 3 is needed to display the installer plan. Run without --dry-run to bootstrap it.\n'
            exit 0
        fi
    done
    install_privilege=()
    if (( EUID != 0 )); then install_privilege=(sudo); fi
    if command -v apt-get >/dev/null 2>&1; then
        "${install_privilege[@]}" apt-get update
        "${install_privilege[@]}" apt-get install -y python3
    elif command -v pacman >/dev/null 2>&1; then
        "${install_privilege[@]}" pacman -S --needed --noconfirm python
    elif command -v dnf >/dev/null 2>&1; then
        "${install_privilege[@]}" dnf install -y python3
    else
        printf 'Install Python 3.10 or newer, then run this script again.\n' >&2
        exit 1
    fi
fi
exec python3 "$install_source/scripts/install.py" "$@"
