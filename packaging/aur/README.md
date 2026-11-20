# Arch Linux (AUR) packages

Two package recipes live here:

- `tompouce/` — the terminal live-coding environment. Source is pinned to a
  commit of <https://github.com/ekollof/tompouce>. Sound banks are **not**
  bundled (they are roughly 3 GiB); install them separately with the upstream
  installer if wanted.
- `libv8/` — a pinned V8 16.0.94 static embedder library for the optional
  JavaScript query worker. It is not used unless a build opts into it.

## Before submitting

1. The `# Maintainer:` field in each `PKGBUILD` is set to Emiel Kollof
   <emiel@kollof.nl>. Keep it if submitting under that AUR account, or change
   it to the account used for submission.
2. Update `_commit`, `_date` and `pkgver` in `tompouce/PKGBUILD` when packaging
   a newer upstream revision. `libv8/PKGBUILD` pins exact depot_tools and V8
   commits.
3. Regenerate `.SRCINFO` after any change:

   ```sh
   cd packaging/aur/tompouce && makepkg --printsrcinfo > .SRCINFO
   cd packaging/aur/libv8 && makepkg --printsrcinfo > .SRCINFO
   ```

4. Each AUR git repository must contain exactly `PKGBUILD` and `.SRCINFO` at
   its root (plus any local files referenced by `source=`). Copy the relevant
   directory there and commit; AUR submission is done from your account.

## Build notes

- `libv8` downloads several GiB of depot_tools/V8 sources and takes about
  25 minutes on a 16-core machine; make sure the build machine has enough disk
  space. Consumers must link with `lld` (V8 16 emits experimental CREL
  relocations); the installed `v8.pc` provides a link script with all required
  static libraries. See `libv8/USAGE.md`.
- The libv8 package disables V8's in-process sandbox (it requires hardened
  libc++ in this pinned build) and the Chromium-specific clang plugins. It is
  intended for a separately confined worker process, not for untrusted code in
  the main engine.
