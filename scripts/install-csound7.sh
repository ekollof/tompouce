#!/usr/bin/env bash
set -euo pipefail
# Pinned upstream native engine; no system library is replaced.
[[ $(uname -s) == Linux && $(uname -m) == x86_64 ]] || { echo 'This binary package requires Linux x86_64' >&2; exit 1; }
csound_prefix=${1:-"$HOME/.local/opt/tompouce-csound7"}
csound_work=$(mktemp -d)
trap 'rm -rf "$csound_work"' EXIT
csound_tag=7.0.0-beta.18
curl -fL --proto '=https' --max-time 120 "https://github.com/csound/csound/releases/download/$csound_tag/csound-linux-minimal-x86_64-$csound_tag.zip" -o "$csound_work/binary.zip"
echo "2c90f6580f1e24bad460b9dc1ab391139df32757024ba9785850cc627a0c04e4  $csound_work/binary.zip" | sha256sum -c -
curl -fL --proto '=https' --max-time 120 "https://codeload.github.com/csound/csound/tar.gz/refs/tags/$csound_tag" -o "$csound_work/source.tar.gz"
echo "e4b02d96ffdef0a19e716bbab5c3626fb945a695307908ecf633f85a5b69dba3  $csound_work/source.tar.gz" | sha256sum -c -
unzip -q "$csound_work/binary.zip" -d "$csound_work"
mkdir "$csound_work/source"
tar -xf "$csound_work/source.tar.gz" -C "$csound_work/source" --strip-components=1
mkdir -p "$csound_prefix/lib" "$csound_prefix/include/csound" "$csound_prefix/plugins"
cp "$csound_work/csound-linux-minimal-x86_64-$csound_tag/libcsound64.so.7.0" "$csound_prefix/lib/"
ln -sfn libcsound64.so.7.0 "$csound_prefix/lib/libcsound64.so"
cp "$csound_work/csound-linux-minimal-x86_64-$csound_tag/plugins/"*.so "$csound_prefix/plugins/"
cp "$csound_work/source/include/"*.h "$csound_prefix/include/csound/"
cat > "$csound_prefix/include/csound/version.h" <<'HEADER'
#ifndef CSOUND_VERSION_H
#define CSOUND_VERSION_H
#define VERSION "7.0"
#define CS_VERSION 7
#define CS_SUBVER 0
#define CS_PATCHLEVEL 0
#endif
HEADER
cat > "$csound_prefix/include/csound/float-version.h" <<'HEADER'
#ifndef USE_DOUBLE
#define USE_DOUBLE 1
#endif
HEADER
cp "$csound_work/source/COPYING" "$csound_prefix/"
echo "Installed Csound $csound_tag in $csound_prefix; reconfigure with cmake --preset default and rebuild with cmake --build --preset default"
