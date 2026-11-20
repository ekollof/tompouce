#!/usr/bin/env bash
# Build a single-file AppImage containing the tompouce binary, its runtime
# files and the shared libraries it needs. Sound banks are not bundled (they
# are several GiB); set TOMPOUCE_BANKS or install them separately.
set -euo pipefail

SOURCE=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
BINARY="${TOMPOUCE_APPIMAGE_BINARY:-$SOURCE/build/tompouce}"
OUT="${1:-$SOURCE/tompouce-x86_64.AppImage}"
TOOL="${APPIMAGETOOL:-$HOME/.cache/appimage-tools/appimagetool-x86_64.AppImage}"
TOOL_URL=https://github.com/AppImage/AppImageKit/releases/download/continuous/appimagetool-x86_64.AppImage
WORK="${TOMPOUCE_APPIMAGE_WORK:-$(mktemp -d)}"
APPDIR="$WORK/AppDir"
KEEP_WORK=0

usage() {
  cat <<'EOF'
Usage: scripts/build-appimage.sh [output.AppImage]

Environment:
  TOMPOUCE_APPIMAGE_BINARY  built engine binary (default: build/tompouce)
  TOMPOUCE_APPIMAGE_WORK    work directory (default: a temporary directory)
  CSOUND_PREFIX             Csound prefix with lib/ and plugins/ to bundle
  APPIMAGETOOL              path to appimagetool (downloaded if missing)
  TOMPOUCE_APPIMAGE_KEEP=1  keep the AppDir after building
EOF
}
if [[ ${1:-} == -h || ${1:-} == --help ]]; then usage; exit 0; fi

if [[ ! -x "$BINARY" ]]; then
  echo "build-appimage: binary not found or not executable: $BINARY" >&2
  exit 1
fi
if [[ ! -x "$TOOL" ]]; then
  mkdir -p "$(dirname "$TOOL")"
  echo "build-appimage: downloading appimagetool to $TOOL" >&2
  curl -fL --retry 2 -o "$TOOL" "$TOOL_URL"
  chmod +x "$TOOL"
fi
if ! command -v convert >/dev/null 2>&1; then
  echo "build-appimage: ImageMagick 'convert' is needed for the icon" >&2
  exit 1
fi

cleanup() {
  if [[ $KEEP_WORK -eq 0 ]]; then rm -rf "$WORK"; else echo "AppDir kept at $APPDIR" >&2; fi
}
trap cleanup EXIT

rm -rf "$APPDIR"
mkdir -p "$APPDIR/usr/bin" "$APPDIR/usr/lib" \
  "$APPDIR/usr/share/icons/hicolor/256x256/apps" "$APPDIR/usr/share/doc/tompouce"

install -m 755 "$BINARY" "$APPDIR/usr/bin/tompouce"

# Bundle Csound when the binary was built with it. Plugins are loaded from a
# runtime path because the AppImage mount point is not known at build time.
bundle_csound=0
csound_prefix="${CSOUND_PREFIX:-}"
if ldd "$BINARY" 2>/dev/null | grep -q libcsound64; then
  if [[ -z $csound_prefix ]]; then
    for candidate in "$HOME/.local/opt/strudelterm-csound7" \
                     "$HOME/.local/opt/tompouce-csound7" /usr/local; do
      if [[ -d $candidate/plugins ]]; then csound_prefix=$candidate; break; fi
    done
  fi
  if [[ -d $csound_prefix/plugins && -d $csound_prefix/lib ]]; then
    bundle_csound=1
    mkdir -p "$APPDIR/usr/share/csound/plugins" "$APPDIR/usr/share/licenses/csound"
    cp -aL "$csound_prefix"/plugins/*.so* "$APPDIR/usr/share/csound/plugins/"
    if [[ -f $csound_prefix/COPYING ]]; then
      cp -a "$csound_prefix/COPYING" "$APPDIR/usr/share/licenses/csound/"
    fi
  else
    echo "build-appimage: warning: binary uses Csound but no prefix with plugins was found" >&2
  fi
fi

# The engine locates scripts/web/examples next to /proc/self/exe, so these
# must be siblings of the real binary inside the AppImage.
files=(
  README.md LICENSE NOTICE
  scripts/companion.py scripts/sound_import.py scripts/query-worker.mjs
  scripts/tompoucectl.py scripts/install-daemon.py scripts/fetch-banks.py
  scripts/install.py scripts/banks.lock.json scripts/gm-webaudiofont.lock.json
  systemd/tompouce-daemon.service.in
  web/index.html web/companion.js web/sounds.js web/companion.css
  web/dist/editor.js web/dist/licenses.txt js/dist/licenses.txt
  thirdparty/STRUDEL-LICENSE thirdparty/quickjs/LICENSE
  thirdparty/tree-sitter-javascript/LICENSE thirdparty/strudel-csound/README.md
  thirdparty/minimp3.h thirdparty/dr_flac.h
  thirdparty/native-synth/README.md thirdparty/native-synth/LICENSE-ZzFX
  thirdparty/native-synth/LICENSE-Strudel thirdparty/native-synth/LICENSE-Kabelsalat
  thirdparty/native-synth/LICENSE-Apache-2.0 thirdparty/native-synth/LICENSE-Phaze
  thirdparty/native-synth/LICENSE-FFT thirdparty/waveforms/NOTICE
)
for file in "${files[@]}"; do
  install -Dm644 "$SOURCE/$file" "$APPDIR/usr/bin/$file"
done
for file in arpoon.json belldub.json bassfugue.json flatrave.json \
            juxundtollerei.json strudeltutor.json; do
  install -Dm644 "$SOURCE/$file" "$APPDIR/usr/bin/examples/$file"
done

# Bundle the shared-library closure, excluding glibc and the loader so the
# AppImage uses the host's glibc.
declare -A seen=()
home_dep=0
queue=("$BINARY")
if [[ $bundle_csound -eq 1 ]]; then
  queue+=("$APPDIR/usr/share/csound/plugins/"*.so*)
fi
while ((${#queue[@]})); do
  item="${queue[0]}"
  queue=("${queue[@]:1}")
  while read -r name arrow path _rest; do
    [[ $name == linux-vdso.so* || $name == libc.so.6 || $name == ld-linux* || \
       $name == libpthread.so.0 || $name == libdl.so.2 || $name == librt.so.1 ]] && continue
    [[ $arrow == '=>' && -n ${path:-} ]] || continue
    [[ -e $path ]] || continue
    [[ -n ${seen[$path]:-} ]] && continue
    [[ $path == "$HOME"/* ]] && home_dep=1
    seen[$path]=1
    target=$(readlink -f "$path")
    cp -aL "$path" "$APPDIR/usr/lib/$(basename "$path")"
    if [[ $target != "$path" ]]; then
      cp -aL "$target" "$APPDIR/usr/lib/$(basename "$target")"
      [[ -n ${seen[$target]:-} ]] || { seen[$target]=1; queue+=("$target"); }
    fi
    queue+=("$path")
  done < <(ldd "$item" 2>/dev/null || true)
done
if [[ $home_dep -eq 1 ]]; then
  echo "build-appimage: warning: a dependency lives under your home directory;" >&2
  echo "  rebuild with -DWITH_CSOUND=OFF for a portable bundle" >&2
fi

cat > "$APPDIR/AppRun" <<'EOF'
#!/bin/sh
HERE=$(dirname "$(readlink -f "$0")")
export LD_LIBRARY_PATH="$HERE/usr/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
if [ -d "$HERE/usr/share/csound/plugins" ]; then
  export TOMPOUCE_CSOUND_PLUGINS="$HERE/usr/share/csound/plugins"
  export OPCODE6DIR64="$TOMPOUCE_CSOUND_PLUGINS"
fi

# Sound banks are not bundled (several GiB). Use an existing install if the
# user has one, honour an explicit TOMPOUCE_BANKS, otherwise fetch the full
# pinned catalog once into the user's data directory using the project's own
# checksummed installer. Re-running after an interruption resumes safely.
if [ -z "${TOMPOUCE_BANKS:-}" ]; then
  if [ -d "$HOME/.local/lib/tompouce/assets/banks" ]; then
    TOMPOUCE_BANKS="$HOME/.local/lib/tompouce/assets/banks"
  else
    BANKS="${XDG_DATA_HOME:-$HOME/.local/share}/tompouce/assets/banks"
    if [ ! -f "$BANKS/.install-receipt.json" ]; then
      echo "Tompouce: first run: downloading sound banks into $BANKS" >&2
      mkdir -p "$(dirname "$BANKS")" || exit 1
      "$HERE/usr/bin/scripts/fetch-banks.py" "$BANKS" || exit 1
    fi
    TOMPOUCE_BANKS="$BANKS"
  fi
  export TOMPOUCE_BANKS
fi
exec "$HERE/usr/bin/tompouce" "$@"
EOF
chmod +x "$APPDIR/AppRun"

cat > "$APPDIR/tompouce.desktop" <<'EOF'
[Desktop Entry]
Type=Application
Name=Tompouce
Comment=Terminal live-coding environment for music
Exec=tompouce
Icon=tompouce
Terminal=true
Categories=AudioVideo;Music;Utility;
EOF
convert "$SOURCE/docs/images/logo.jpg" -resize 256x256 \
  "$APPDIR/usr/share/icons/hicolor/256x256/apps/tompouce.png"
cp "$APPDIR/usr/share/icons/hicolor/256x256/apps/tompouce.png" "$APPDIR/tompouce.png"

cat > "$APPDIR/usr/share/doc/tompouce/README.txt" <<'EOF'
Tompouce AppImage

Run with a terminal. Sound banks are too large to bundle: the first run fetches
the full pinned catalog (as install.sh --all-banks) into
$XDG_DATA_HOME/tompouce/assets/banks using the checksummed, resumable installer.
Set TOMPOUCE_BANKS to use a different bank directory and skip the fetch, or use
an existing ~/.local/lib/tompouce/assets/banks install. All other runtime files
(scripts, web assets, licenses) are inside the AppImage next to the executable.
When the packaged binary links libcsound64, its plugins and licence are bundled
too and AppRun points Csound at them through TOMPOUCE_CSOUND_PLUGINS.
EOF

rm -f "$OUT"
echo "build-appimage: creating $OUT" >&2
APPIMAGE_EXTRACT_AND_RUN=1 "$TOOL" "$APPDIR" "$OUT"
echo "build-appimage: done: $OUT" >&2
