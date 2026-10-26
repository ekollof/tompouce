# Getting started

## Install

The installer targets Linux x86_64 and supports package plans for Arch/Omarchy, Debian/Ubuntu and Fedora. Use a UTF-8 terminal and run the installer as your normal user:

```sh
git clone git@github.com:ekollof/tompouce.git
cd tompouce
./install.sh
~/.local/bin/tompouce
```

A plain `git clone https://github.com/ekollof/tompouce.git` works anonymously; an authenticated `gh repo clone ekollof/tompouce` works too.

The default installation puts the runtime in `~/.local/lib/tompouce` and its launcher in `~/.local/bin`. The installer uses sudo for distro dependencies, builds in an isolated directory, and installs pinned banks separately. Node/npm is unnecessary for normal installation because JavaScript bundles are committed.

Default banks include Dirt-Samples, Salamander piano, Tidal drum machines, the uzu drumkit, AKWF wavetable banks and gm webaudiofont packs, about 473 MiB. `--all-banks` adds VCSL and mridangam, bringing the total to roughly 3 GiB. Installed banks work offline. Debian/Fedora package plans have automated coverage; actual package-manager execution still needs validation on those distributions.

Useful installer options:

| Option | Purpose |
| --- | --- |
| `--dry-run` | Inspect the dependency and installation plan |
| `--prefix /absolute/path` | Choose another installation root |
| `--skip-deps` | Use dependencies already installed |
| `--skip-banks` | Preserve existing banks or use built-in synths only (`gm_*` then fetches packs on first play) |
| `--no-browser` | Omit browser installation |
| `--jobs 4` | Bound build parallelism |

## Make your first sound

Type this into the left editor pane:

```js
setcpm(108 / 4)
note("c4 e4 g4 c5").s('triangle').gain(0.4)
```

Press Ctrl+Enter or Alt+Enter, the same keys as the Strudel REPL, or press F5. That evaluates the pattern and starts playback. Ctrl+. or Alt+. stops. Ctrl+P toggles play and stop. Change a note and evaluate again. F1 opens the keybinding reference; Escape returns focus from the right pane to the editor.

For guided practice, press F2, enter `/tutor`, then Enter. See [Live coding and Strudeltutor](https://github.com/ekollof/tompouce/wiki/Live-Coding-and-Strudeltutor).

## Update

Save your work, update the checkout and rerun the installer:

```sh
git pull --ff-only
./install.sh --skip-deps --skip-banks --no-browser --jobs 4
```

This preserves existing banks and skips dependency/browser installation. Omit those skip options when you need updated dependencies or newly added banks. Restart Tompouce normally to load a new executable. Installation does not restart a running session or replace saved tutorial progress.

For source builds, see [Development](https://github.com/ekollof/tompouce/wiki/Development). For sound output problems, see [Troubleshooting](https://github.com/ekollof/tompouce/wiki/Troubleshooting).
