# Development

## Build and test

The project uses C11, CMake 3.20+ and Ninja. From a complete checkout:

```sh
cmake --preset default
cmake --build --preset default --parallel 4
ctest --preset default
./build/tompouce
```

Build outputs and generated headers stay under `build/`; the previous hand-maintained Makefile is removed. CMake generates `compile_commands.json`. Requirements and distro package names are in the [README build section](https://github.com/ekollof/tompouce/blob/master/README.md#build).

Optional backends use `WITH_PIPEWIRE`, `WITH_PULSE`, `WITH_SAMPLERATE`, `WITH_FLUIDSYNTH` and `WITH_CSOUND`. Private dependencies use `CSOUND_PREFIX` and `TREE_SITTER_PREFIX`. Normal builds use committed JavaScript bundles; Node/npm is needed only to rebuild bundles or run browser-based tooling.

Focused commands:

```sh
ctest --test-dir build -R javascript --output-on-failure
ctest --test-dir build -R examples --output-on-failure
cmake --build build --target test-sanitize
cmake --build build --target test-terminal-sanitize
cmake --build build --target test-audio-backends
```

CTest covers native behavior, 31 published examples/1,207 stored reference events, Csound, installer/import/companion checks and terminal PTYs. Device tests use owned silent streams rather than changing a live composition. See [audio comparisons](https://github.com/ekollof/tompouce/blob/master/docs/audio-comparison.md) for maintained regression gates and remaining diagnostic differences.

## Code map

| Area | Main files |
| --- | --- |
| Editor/UI | `src/tui.c`, `src/tui_terminal.h`, `src/tui_vi.h`, `src/tui_commands.h`, `src/tui_completion.h` |
| Pattern runtime | `js/runtime.mjs`, `src/javascript.c`, `src/js_audio.c` |
| Scheduling | `src/track.c`, `src/scheduler.c` |
| Native audio | `src/audio.c`, `src/dsp.c`, `src/audio_output.c` |
| Sources | `src/sampler.c`, `src/websamp.c`, `src/native_synth.c`, `src/gm_synth.c` |
| Browsing/imports | `src/sounds.c`, `src/sound_store.c`, `scripts/sound_import.py` |
| Visuals | `src/visual.c`, `src/visual_render.c`, `src/terminal_graphics.c` |
| Integrations | `src/mcp.c`, `src/companion.c`, `src/midi.c`, `src/osc_output.c` |
| Installer | `scripts/install.py`, `scripts/banks.lock.json` |

JavaScript, sample decoding and prepared-audio work stay outside the audio callback. The callback consumes prepared data and runs native DSP. Keep resource bounds, ownership and source-location mapping intact when changing these paths.

Read [AGENTS.md](https://github.com/ekollof/tompouce/blob/master/AGENTS.md) for repository editing rules and recent implementation notes. JavaScript Strudel is the only source language; the historical C statement/mini parser and line dialect have been removed. Preserve vendored licenses and generated completion/runtime metadata. Discuss ideas in [Discussions](https://github.com/ekollof/tompouce/discussions), and include relevant validation with contributions.
