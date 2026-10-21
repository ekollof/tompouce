# CPU optimization benchmark

Measured on 2026-10-21, AMD Ryzen 7 5800X3D, GCC 16.2.1, Linux x86-64.
This is a device-free optimization experiment, not a backend underrun or audio
parity gate. The initial experiment changed no production flags; the subsequent
runtime-dispatched implementation and its measurements are described below.
All four focused regressions (`audio`, `audio_performance`, `dsp`, `room`)
passed after adding the benchmark, as did the preparation profiling fixture.

## Method

`tests/bench_simd.c` includes the production callback and convolution sources,
following the existing offline harness convention. Other modules, including
`dsp.c`, remain linked from the normal `-O2` core. Each alternative recompiles
only that benchmark translation unit:

- baseline: normal Release `-O2`;
- portable alternative: `-O3 -ffp-contract=off`;
- AVX2 alternative: `-O3 -mavx2 -ffp-contract=off`.

There are no handwritten intrinsics, relaxed math flags or FMA requirements.
Baseline C can already contain compiler-generated SSE instructions; it is not
an artificially de-vectorized baseline. This experiment measures compiler
targeting, not the incremental benefit of handwritten intrinsics.

Callback cases keep 1, 32 or 128 voices alive, warm up 100 blocks, then render
1,500 blocks of 128 stereo frames (4.354 seconds at 44.1 kHz). Effects combine
low-pass and phaser. Convolution cases fill history before timing 1,000 blocks
(2.902 seconds), then separately measure 1,000 production tail-block calls.
The FFT case performs 200,000 forward/inverse 256-point pairs, resetting input
each time. Kernel preparation is outside callback timing.

The table reports medians of three process runs, alternating variant order.
Timings use thread CPU time to exclude scheduling delays. Every timed block
also has a CPU timer, adding small measurement overhead. Maximum observed block
CPU time is emitted, but is neither a wall-clock latency bound nor a deadline
guarantee. Processes were not pinned and CPU frequency was not fixed. Results
are specific to this machine; cache behavior and scaling can differ elsewhere.

## Results

Total thread CPU time in milliseconds for each workload:

| Workload | Baseline | Portable O3 | AVX2 | Baseline / AVX2 |
| --- | ---: | ---: | ---: | ---: |
| 1 sine voice | 465.2 | 351.1 | 338.7 | 1.37x |
| 32 sine voices | 765.0 | 649.8 | 625.8 | 1.22x |
| 128 sine voices | 1851.0 | 1697.9 | 1709.0 | 1.08x |
| 32 voices, filter + phaser | 1028.3 | 916.2 | 886.2 | 1.16x |
| 128 voices, filter + phaser | 2933.3 | 2831.2 | 2831.1 | 1.04x |
| 1 looping stereo sample | 479.9 | 350.3 | 369.0 | 1.30x |
| 32 looping stereo samples | 688.5 | 554.4 | 573.3 | 1.20x |
| 128 looping stereo samples | 1662.7 | 1421.2 | 1503.7 | 1.11x |
| Convolution, 128-frame head only | 21.45 | 21.31 | 21.33 | 1.01x |
| Convolution, 0.1-second IR | 42.63 | 40.58 | 35.04 | 1.22x |
| Convolution, 1-second IR | 128.76 | 120.40 | 76.15 | 1.69x |
| Convolution, 3-second IR | 311.29 | 291.26 | 162.01 | 1.92x |
| Tail blocks only, 3-second IR | 291.78 | 274.58 | 140.73 | 2.07x |
| 200,000 FFT round trips | 1161.45 | 1012.13 | 984.45 | 1.18x |

The three-second convolution changes from about 0.311 ms to 0.162 ms per
128-frame block, whose real-time budget is 2.902 ms: about 10.7% versus 5.6%
of one core for this isolated effect. Several simultaneous convolvers can
therefore make this optimization useful, though multi-orbit scaling was not
measured here. The direct convolution head shows essentially no benefit.

GCC's `-fopt-info-vec-all` report confirms the frequency-bin complex
multiply/accumulate loop in `src/convolution.c:153` vectorizes with 32-byte
vectors under AVX2. The FFT butterfly and direct-head reduction loops do not
vectorize as loops in that build. The FFT result includes other code-generation
changes, not proof of a vectorized FFT algorithm.

Looping samples use an owned one-second stereo WAV, decoded before timing;
assertions verify all requested voices remain active after measurement. AVX2
was about 3–6% slower than portable O3 on those sample cases. Wider targeting
does not automatically improve all workloads.

The no-inline sampling run attributed about 18% of its fixed workload mix to
`dsp_frequency`, 14% to `transform`, 13% each to callback self work and
`dsp_process`, 11% to `process_block`, 11% to `native_reverb`, and 6% to
`orbit_delay`. These percentages include the deliberately repeated standalone
FFT/convolution benchmarks and must not be presented as a normal song profile.
The callback invokes room/delay processing for all 12 orbits even in these dry
cases. Investigating safe bypass of inactive effects and repeated per-voice
control work could yield portable improvements; tail retention and live
automation must remain correct. Profile `dsp_frequency` separately before
choosing between a no-FM fast path and vectorizing active FM operators.

Printed output checksums agree across variants for these cases. Checksums are
only a sanity check: they do not establish sample-by-sample equivalence or
replace existing web/native audio gates.

## Preparation

The existing `test_prep_profile` passed with owned sample fixtures. One run
measured approximately 12.35 ms CPU for a cold stretch, 0.61 ms for cold sample
convolution FFT preparation, and 2.93 ms FFT plus 2.25 ms synthesis for a cold
generated room. Cold WAV decode was 4.13 ms and installed MP3 decode 14.52 ms.
Four independent stretch jobs took 29.1/14.6/8.4 ms wall time with 1/2/4 workers.
These are workload-specific observations, not SIMD comparisons. Stretch is a
reasonable second profiling target for preparation latency, after a callback
convolution prototype; decode measurements do not justify replacing decoders.

## Implemented convolution dispatch

`src/convolution_mix.c` now supplies portable C, SSE2, AVX and AVX2 accumulation
kernels. Only that translation unit uses O3 with FP contraction disabled; each
targeted function is restricted to its declared ISA. Production settings for the rest
of the engine are unchanged. Compiler runtime detection checks usable CPU/OS
features once, before audio startup or offline kernel preparation. An atomic
backend pointer defaults to C even before initialization; the callback reads it
once per convolution block, with no feature detection or initialization there.

`TOMPOUCE_SIMD=auto` (default) selects AVX2, then AVX, then SSE2, then C.
`c`, `sse2`, `avx` and `avx2` force a specific implementation at initialization.
Unsupported explicit requests and invalid values fall back to C with a console
diagnostic. There is no runtime reset or reconfiguration API. SSE2 is the x86-64
baseline and may perform like the compiler-vectorized C path.
[MMX provides integer SIMD](https://www.intel.com/content/www/us/en/support/articles/000005779/processors.html)
and has no suitable floating-point kernel here.

Three-run median CPU times for the actual dispatched implementation, with the
rest of the production build at O2 and 1,000 blocks as above:

| Workload | Forced C (ms) | Forced AVX2 (ms) | Speedup |
| --- | ---: | ---: | ---: |
| Head only | 21.52 | 21.67 | 0.99x |
| Full convolution, 0.1-second IR | 42.77 | 38.09 | 1.12x |
| Full convolution, 1-second IR | 127.60 | 79.65 | 1.60x |
| Full convolution, 3-second IR | 313.69 | 170.17 | 1.84x |
| Tail blocks only, 3-second IR | 296.38 | 148.51 | 2.00x |

The targeted object contains YMM multiply/add/subtract operations and no fused
multiply/add instructions. Dedicated tests compare exact accumulation bits and
streaming PCM, including vector remainders, unaligned buffers, history-ring
wraparound, effect tails, replacement, concurrent initialization, invalid
overrides and a build with SIMD disabled. ASan/UBSan runs force both backends.
These synthetic checks supplement rather than replace web/audio comparisons.

Implementation validation passed all 35 CTests and both forced-backend sanitizer
runs. The actual web audio matrix passed 43 gates with two existing diagnostic
cases; re-rendering all 45 native cases with forced C produced byte-identical
PCM to automatic AVX2 selection. The owned PipeWire/null-sink preparation-load
matrix passed all six workloads, including concurrent orbit effects and voice
saturation: no observed missed cycles, stream drops or render overruns. Its
injected 100 ms stall positive control recorded one 102.67 ms gap. These are
selected workloads, not proof of unrestricted multi-orbit scaling or guarantees
for other machines/backends. Reports are under `/tmp/tompouce-simd-audio` and
`/tmp/tompouce-simd-backend` for this run, not permanent test inputs.
The verified implementation was installed in `~/.local/lib/tompouce/tompouce`;
installed companion startup/cleanup checks passed under both automatic selection
and forced C. The user's running session and system Notcurses were untouched.

## AVX and SSE2 extension

The lower-ISA kernels explicitly disable AVX2/FMA (AVX path) or SSE3 and newer
extensions including AVX/FMA (SSE2 path). Inspection of the GCC object confirms
AVX floating-point YMM operations without AVX2 instructions, and legacy SSE/SSE2
XMM operations without AVX in the SSE2 function. The module also compiles with
Clang under warning-as-error settings.

Three-run median CPU times for 1,000 full three-second-IR convolution blocks
were 326.3 ms C, 326.5 ms SSE2, 174.2 ms AVX and 178.2 ms AVX2 on the same
Ryzen host. The two 256-bit paths give similar throughput; this run does not
establish an AVX-versus-AVX2 ranking or performance on older hardware.

After this extension, all 12 focused audio/convolution/preparation checks and
ASan/UBSan runs forcing C, SSE2, AVX and AVX2 passed. Each new backend reproduced
the prior native PCM hashes for five custom-IR/generated-room/Arpoon cases,
including the existing diagnostic room cases. Selection-policy tests enumerate
all feature masks, including AVX-without-AVX2, SSE2-only and no-SIMD, plus
unsupported explicit overrides and a SIMD-disabled build. Kernel execution was
tested on the current host; policy simulation is not an old-CPU hardware test.
The extension is installed; companion startup/cleanup checks against the
installed executable passed with both `TOMPOUCE_SIMD=avx` and `sse2`.

## Further optimization

The compiler already finds useful vectorization in the narrow accumulation
kernel; handwritten intrinsics are not needed for this improvement. Keep
partition accumulation order and avoid FMA/reassociation.

Maintain initialization-time CPU/OS selection and the force-implementation
override. No allocation or dispatch reconfiguration belongs in the callback.
Further kernels need PCM comparisons, bounds/sanitizer checks, multi-orbit loads
and backend deadline measurements. These results do not justify a global AVX2
build or a global optimization-flag change.

FFT optimization is a separate candidate: the measured AVX2 increment over
portable O3 is small, and a different FFT implementation needs independent
numerical validation. Synth callback gains mostly come from portable compiler
optimization in this experiment; the linked DSP module was not retargeted, so
these results neither prove nor disprove SIMD gains inside its filters or FM.

## Reproduce

```sh
cmake --preset default
cmake --build build --target bench_simd bench_simd_o3 bench_simd_avx2 bench_simd_profile --parallel 4
./build/tests/bench_simd
./build/tests/bench_simd_o3
./build/tests/bench_simd_avx2
./build/tests/bench_simd --samples
./build/tests/bench_simd_o3 --samples
./build/tests/bench_simd_avx2 --samples
./build/tests/test_prep_profile
TOMPOUCE_SIMD=c ./build/tests/bench_simd --convolution
TOMPOUCE_SIMD=avx2 ./build/tests/bench_simd --convolution
cmake --build build --target test-simd-sanitize --parallel 4
TOMPOUCE_SIMD=sse2 ./build/tests/bench_simd --convolution
TOMPOUCE_SIMD=avx ./build/tests/bench_simd --convolution
```

The first table records the pre-dispatch experiment; current benchmark builds
use the new accumulation module. Use the forced-backend commands to reproduce
the dispatched-kernel comparison, without retargeting the callback or FFT.
Alternative targets currently require GCC; the AVX2 target is offered only on
x86-64 and must only be executed on a CPU/OS supporting AVX2. It is an explicit
benchmark, not automatic production dispatch. Repeat the commands serially and
alternate ordering. Do not rebuild executables while measurement runs use them.
For compiler vectorization diagnostics, add `-fopt-info-vec-all` when compiling
the benchmark translation unit.

`perf` was unavailable. The optional `bench_simd_profile` uses `gprof` with
inlining and IPA argument specialization disabled in the harness to retain
function names. Run it in an owned temporary working directory, then pass that
directory's binary `gmon.out` and the executable to `gprof`. Sampling is coarse,
instrumentation changes costs, and linked core modules lack call instrumentation;
use it to locate candidates, never as the comparative timing source.
