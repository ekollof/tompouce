#!/bin/sh
# Run CTest without emitting systemd-coredump journal entries for expected
# test assertion failures. `ulimit -c 0` alone still lets systemd-coredump
# record a metadata-only crash, which Omarchy's crash watcher announces.
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
source_file="$root/scripts/no-core.c"
runtime_dir=${XDG_RUNTIME_DIR:-/tmp}
helper="$runtime_dir/tompouce-no-core-$(id -u).so"

if [ ! -f "$helper" ] || [ "$source_file" -nt "$helper" ]; then
    temporary=$(mktemp "$runtime_dir/tompouce-no-core.XXXXXX")
    trap 'rm -f "$temporary"' EXIT HUP INT TERM
    ${CC:-cc} -shared -fPIC -O2 -o "$temporary" "$source_file"
    chmod 600 "$temporary"
    mv -f "$temporary" "$helper"
    trap - EXIT HUP INT TERM
fi

LD_PRELOAD="$helper${LD_PRELOAD:+:$LD_PRELOAD}"
export LD_PRELOAD
# Isolate the user prebake store: enabled prebakes (gain curves, default
# gains) would otherwise leak into every render and test. Tests that need
# prebakes point XDG_CONFIG_HOME at their own directory.
isolated_config=$(mktemp -d "${TMPDIR:-/tmp}/tompouce-test-config.XXXXXX")
trap 'rm -rf "$isolated_config"' EXIT HUP INT TERM
XDG_CONFIG_HOME="$isolated_config"
export XDG_CONFIG_HOME
exec ctest --test-dir "$root/build" "$@"
