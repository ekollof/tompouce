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
cmake --build build --target test-simd-sanitize
cmake --build build --target test-terminal-sanitize
cmake --build build --target test-audio-backends
```

CTest covers native behavior, 31 published examples/1,207 stored reference events, Csound, installer/import/companion checks and terminal PTYs. Device tests use owned silent streams rather than changing a live composition. See [audio comparisons](https://github.com/ekollof/tompouce/blob/master/docs/audio-comparison.md) for maintained regression gates and remaining diagnostic differences.

## CPU dispatch and benchmarks

Convolution spectrum accumulation selects AVX2, AVX, SSE2, then portable C
according to CPU/OS support. Selection happens once before rendering; the audio
callback does no feature detection. Only the kernel module uses targeted
optimization, without global AVX requirements, fused operations or relaxed math.

`TOMPOUCE_SIMD=auto` is the default. Set `c`, `sse2`, `avx` or `avx2` to test a
specific implementation; unsupported explicit requests fall back to C. Exact
kernel/streaming PCM comparisons, simulated older feature sets and a
SIMD-disabled build cover fallback behavior. `test-simd-sanitize` forces all
four implementations under ASan/UBSan (unavailable ones safely use C).

For device-free measurements:

```sh
cmake --build build --target bench_simd --parallel 4
TOMPOUCE_SIMD=c ./build/tests/bench_simd --convolution
TOMPOUCE_SIMD=sse2 ./build/tests/bench_simd --convolution
TOMPOUCE_SIMD=avx ./build/tests/bench_simd --convolution
TOMPOUCE_SIMD=avx2 ./build/tests/bench_simd --convolution
```

Run serially and compare repeated runs. AVX and AVX2 were roughly 1.9x faster
than C for one long-convolution workload on a Ryzen 5800X3D; SSE2 performed like
the compiler-vectorized C path. This is not an engine-wide speedup or a latency
guarantee for other CPUs. See the [benchmark report](https://github.com/ekollof/tompouce/blob/master/docs/simd-benchmark.md)
for workloads, PCM validation and backend checks. MMX has no suitable
floating-point implementation for this kernel.

## Code map

| Area | Main files |
| --- | --- |
| Editor/UI | `src/tui.c`, `src/tui_terminal.h`, `src/tui_vi.h`, `src/tui_commands.h`, `src/tui_completion.h` |
| Pattern runtime | `js/runtime.mjs`, `src/javascript.c`, `src/js_audio.c` |
| Scheduling | `src/track.c`, `src/scheduler.c` |
| Native audio | `src/audio.c`, `src/dsp.c`, `src/audio_output.c` |
| Convolution and CPU dispatch | `src/convolution.c`, `src/convolution_mix.c` |
| Sources | `src/sampler.c`, `src/websamp.c`, `src/native_synth.c`, `src/gm_synth.c` |
| Browsing/imports | `src/sounds.c`, `src/sound_store.c`, `scripts/sound_import.py` |
| Visuals | `src/visual.c`, `src/visual_render.c`, `src/terminal_graphics.c` |
| Integrations | `src/mcp.c`, `src/companion.c`, `src/midi.c`, `src/osc_output.c` |
| Installer | `scripts/install.py`, `scripts/banks.lock.json` |

JavaScript, sample decoding and prepared-audio work stay outside the audio callback. The callback consumes prepared data and runs native DSP. Keep resource bounds, ownership and source-location mapping intact when changing these paths.

Read [AGENTS.md](https://github.com/ekollof/tompouce/blob/master/AGENTS.md) for repository editing rules and recent implementation notes. JavaScript Strudel is the only source language; the historical C statement/mini parser and line dialect have been removed. Preserve vendored licenses and generated completion/runtime metadata. Discuss ideas in [Discussions](https://github.com/ekollof/tompouce/discussions), and include relevant validation with contributions.
