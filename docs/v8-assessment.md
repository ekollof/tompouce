# V8 suitability assessment

V8 is a credible candidate for an **optional, isolated pattern-query worker**.
The measured GC events were small enough for the tested scheduling workload.
Production adoption still needs a confined native host and IPC measurements;
this assessment does not implement or select a new backend.

## Scope and reproduction

Measured on 2026-10-27, Linux x86-64, 16 available CPUs, Node 26.12.1 with V8
14.6.202.34-node.40. The same current runtime bundle runs without any QuickJS
native accelerators. Original public Bakery sources and hashes are unchanged.
The tests own their child processes and open no audio device or user session.

```sh
node scripts/assess-v8.mjs /tmp/tompouce-v8-assessment
TOMPOUCE_COMPARE_FILTER=bakery- TOMPOUCE_RENDER_AUDIO=./scripts/render-v8-events.mjs \
  node scripts/compare-audio.mjs /tmp/tompouce-v8-bakery
TOMPOUCE_PROFILE_RUNS=3 TOMPOUCE_PROFILE_WINDOWS=16 \
  node scripts/profile-pattern-query.mjs
```

`render-v8-events.mjs` is an event-only adapter for the existing comparison
harness. It rejects source outside the pinned Bakery corpus and accepts no PCM
requests. It is a diagnostic, not an untrusted-code service. No package versions,
production query limits or engine settings change.

The V8 context uses `vm.constants.DONT_CONTEXTIFY`, which avoids Node's
contextified-global wrapper. The first prototype using that wrapper timed out
on the cold whole-cycle query. This context choice matters: ordinary Node `vm`
measurements need not represent a bare embedded V8 global. The prototype uses
Node's V8 platform and synthetic slider bindings; native host callbacks, IPC and
the audio-engine integration are **not measured**. `node:vm` is not a security
boundary, even with Node globals absent. [Node VM documentation](https://nodejs.org/api/vm.html)

## Query latency and GC

Each normal run uses a fresh child, a measured 128 MiB V8 heap limit
(`--max-old-space-size=104 --max-semi-space-size=8` on this build), a cold
compile and a 200 ms VM query timeout. There is no forced GC or hidden song
warmup. The reported query duration includes runtime event serialization and
host JSON decoding; the VM watchdog covers evaluation and its microtasks.
The JSON decoding is outside that watchdog. Engine bootstrap/context evaluation
was about 44–45 ms and song compilation 24–25 ms; process startup and initial
bundle-script compilation are additional costs.

| Original song ID | Cold whole-cycle query | Median over 96 cycles | Worst over 96 cycles | Worst with 32 MiB retained |
| --- | ---: | ---: | ---: | ---: |
| 13587 | 132.2 ms | 102.0 ms | 135.8 ms | 124.2 ms |
| 78179 | 137.4 ms | 103.3 ms | 137.4 ms | 122.9 ms |

Cycles rotate through 0–31, rather than repeatedly querying only cycle zero.
An additional 32 whole-cycle queries run with approximately 32 MiB of retained
number arrays. All these queries complete under 200 ms on the normal affinity.
The current QuickJS renderer still times out at 200 ms for both original songs.
Its earlier 268 ms compile-stage probe measures a different entry point and
must not be treated as a directly paired speedup ratio for this table.

The scheduler normally requests 50 ms of musical time, not a whole cycle.
For 512 such windows per song at 0.5 cps:

| Song ID | Median | p99 | Worst | Largest GC event during these queries |
| --- | ---: | ---: | ---: | ---: |
| 13587 | 4.84 ms | 7.02 ms | 7.65 ms | 0.78 ms |
| 78179 | 4.86 ms | 7.41 ms | 7.70 ms | 0.85 ms |

Across the normal whole-cycle and pressure phases, the largest GC event during
a query was 1.61 ms; the largest observed GC event including bootstrap/setup
was 2.90 ms. Both minor and major/incremental collections occurred, with roughly
4,600 recorded events per run. These are Node GC performance entries, not a
hard upper bound on every V8 pause or on background CPU consumption. They are
already reflected in the measured query times.

A separate small QuickJS compile-stage probe of the first sixteen 50 ms windows,
repeated three times per song, measured a 35 ms median and maxima of 91/92 ms.
That probe uses a coarse wall clock and raw `queryArc`, without event JSON
conversion; its sampling coverage differs from the V8 run. It nevertheless
shows why the scheduling-shaped workload matters alongside the full-cycle gate.

## CPU and JIT sensitivity

Restricting a fresh V8 child to one CPU caused its first whole-cycle query to
hit 200 ms. V8 therefore does not universally cure the cold-query deadline.
A separate fresh one-CPU run starting with actual playback-shaped windows
completed its first window in 33.4 ms, median 6.40 ms and worst 33.4 ms over
128 windows. After that real query workload, 32 whole cycles had a 129.3 ms
median and 185.5 ms maximum. The largest GC event was 7.45 ms.

With JIT disabled, eight whole-cycle queries had a 400.1 ms median and 503.1 ms
maximum. This diagnostic used a clearly separate 1,000 ms timeout; all eight
exceeded the production 200 ms threshold. JITless playback windows were about
20.4 ms median in a small 16-window sample. The performance case for V8 depends
on enabling its JIT. Do not hide cold-start problems by prequerying arbitrary
songs: that can change stateful callbacks and random/control behavior.

## Event compatibility

The maintained strudel.cc comparison harness reports **41 musical event matches
and one empty pattern** across all 42 Bakery compositions. Both heavy songs
match all 398 events over four cycles. The production QuickJS pass had 39
matches, the empty pattern and the two timeouts. This establishes selected
musical-event compatibility, not full-song PCM, browser APIs, live-control IPC,
loader-side-effect or complete host-feature parity.

## Resource limits and failure behavior

* Infinite synchronous code, an infinite microtask body, a self-queuing promise
  chain and `Atomics.wait` were
  interrupted at about 200.3 ms. A subsequent scalar evaluation returned 42.
  This tests Node's watchdog, not a native C++ watchdog. Production must keep
  a watchdog active through microtask checkpoints and bound every host call.
  V8 provides cross-thread `TerminateExecution` and `RequestInterrupt` APIs.
  [V8 isolate API](https://v8.github.io/api/head/classv8_1_1Isolate.html)
* Managed-heap exhaustion caused the owned child to abort with `SIGABRT`.
  Diagnostic core dumps were disabled in that child. V8's heap-limit contract
  explicitly permits a fatal OOM when collection and the near-limit callback
  cannot make space. This is a decisive reason to isolate it from the audio
  engine process. [V8 resource constraints](https://v8.github.io/api/head/classv8_1_1ResourceConstraints.html)
* A child with a 56 MiB managed-heap limit successfully retained about 96 MiB of
  typed-array storage: managed heap used about 6.8 MiB while RSS reached about
  153 MiB. A heap limit alone does not bound external buffers or process memory.
* Normal V8 RSS was about 113–114 MiB, rising to 153–155 MiB under retained
  pressure, with observed peaks around 157–158 MiB. The two failing QuickJS
  full-cycle subprocesses peaked at about 34–36 MiB. These are different workloads
  and processes, but V8's memory cost is material.

The tested Node binary has V8's heap sandbox and pointer compression disabled.
It is not evidence for the behavior of a standalone build with those features
enabled. V8's heap sandbox also does not replace OS-level file/network/process
confinement. A sandbox-enabled V8 build can reserve a very large virtual address
cage; the Csound worker's existing 512 MiB `RLIMIT_AS` cannot simply be reused.
[V8 sandbox architecture](https://github.com/v8/v8/blob/main/docs/sandbox/architecture.md)

## Proposed integration and acceptance gates

Keep QuickJS available. Prototype a separately spawned C++ V8 worker behind
the opaque `JsProgram` interface, with a C-facing parent API and bounded private
IPC. Audio rendering, accepted events, history and effect tails remain native.
Use the same bundle and pinned packages, initially without porting the five
QuickJS-specific accelerators. The nine functional host callbacks still need
real bindings: logging, sliders, MIDI snapshots, prebakes and Csound operations.
Confined local-module resolution and bounded promise-job pumping also need
native implementations. The QuickJS number parser/formatter used by native audio is a separate utility;
it need not be removed to add another query backend.

Before enabling it for users:

1. Build and package the standalone V8/C++ shim with pinned dependencies and
   all runtime notices. No standalone V8 library is installed on this machine;
   this assessment has not tested that build. V8 supplies a C++ embedding API,
   and an isolate may be entered by one thread at a time.
   [Embedding guide](https://v8.dev/docs/embed)
2. Validate bounded IPC, generation/source identity and end-to-end query
   deadlines, including serialization and process scheduling. Preserve 2 s
   evaluation and 200 ms query budgets, failed-evaluation retention, accepted
   notes and live tails. Worker failure must be observable without crashing the
   engine or silently restarting/re-evaluating a song.
3. Establish OS confinement across **all** V8 threads, including background
   compiler/GC workers. Audit JIT executable mappings and thread initialization;
   do not broaden the Csound sandbox wholesale. Broker permitted file/URL
   operations through the existing guarded parent entry points.
4. Bound ArrayBuffer/external allocation separately and establish a process
   memory policy compatible with V8's virtual reservations. Exercise OOM,
   malformed frames, watchdog expiry and worker death without touching playback.
5. Run the live-control, transport, loader, visual-generation and MIDI reference
   suites, then owned audio-backend continuity tests under GC/JIT/compile load.
   Compile a candidate without starving the currently playing program. Measure
   cold program swaps, low-CPU cases and actual IPC p99 tails before choosing a
   default backend.

The evidence supports funding that prototype. It does not support replacing
the production backend directly with the Node diagnostic or declaring GC-free,
hard-real-time or complete REPL parity.

## Worker transport spike (2026-11-04)

The prerequisite that does not need a V8 build was measured with the existing
diagnostic host: a spawned worker process (spawn, bootstrap, compile) becomes
ready for its first query in 72–107 ms, and a warm 50 ms scheduling window
round-trips through the pipe, query and JSON in 7.8 ms median against 8.9 ms
in-process. Transport overhead is therefore small next to the 200 ms query
budget and the 2 s evaluation budget, provided compilation never runs on the
audio path. This supports the separate-process design; it does not measure a
native C++ host, watchdog or confinement.

## Opt-in Node worker prototype (2026-11-04)

`src/js_worker.c` + `scripts/query-worker.mjs` implement the transport the
migration needs, without changing the production backend: the parent sends the
embedded bundle once, then bounded newline-delimited JSON compile/query frames
with per-request deadlines. `tests/test_js_worker.c` checks that the worker and
the QuickJS backend produce identical first-event values and event counts for
three patterns, that syntax errors and oversized sources are rejected, that an
infinite loop is interrupted by the worker's own watchdog, and that a dead or
missing worker is reported instead of hanging or killing the caller. `SIGPIPE`
is blocked around writes so a worker that dies mid-request cannot terminate the
engine.

`TOMPOUCE_QUERY_BACKEND=node` (or `v8`) now delegates engine queries to that
worker. QuickJS remains the default; with the variable unset nothing changes.
Delegation is deliberately narrow: a program is compiled by both engines, and
only plain musical patterns are queried through the worker. Programs that need
QuickJS-only host state stay native — visual attachments (`_scope`, `pianoroll`,
Hydra placeholders), prebakes, Csound orchestras, live sliders and `midin`
input readers. Compile-time console output still comes from QuickJS, while
query-time console lines are forwarded from the worker, so warnings appear once.
One shared worker holds the most recently compiled compatible source; other
programs (song mode, older tracks) keep querying QuickJS instead of
recompiling per query. Each evaluation carries a unique compile serial, so
re-evaluating identical source starts fresh worker state rather than continuing
the previous evaluation, and a program replaced by a newer evaluation falls
back to its retained QuickJS state. A worker timeout, death or desynchronized reply is
reported and the affected program falls back to its QuickJS evaluation, which
was never discarded. That fallback resumes a pattern whose internal state
(random streams, stateful callbacks) has not advanced with the worker's, so it
is an emergency continuation rather than a seamless handoff; a later evaluation
returns to a consistent state.

The worker now runs with a bounded OS footprint: its own process group, a
parent-death signal, `PR_SET_NO_NEW_PRIVS`, no core dumps, a 3,600 s CPU limit,
a 64-descriptor limit and a 2 GiB address-space limit (Node reserves about
1.1 GiB before running any song). It also receives a fixed minimal environment
(`PATH`, `LANG`, `LC_ALL`, `TMPDIR`), so `NODE_OPTIONS`, `NODE_PATH` and similar
variables cannot inject code; `tests/test_js_worker.c` checks both the limits
via `/proc/<pid>/limits` and that a `--require` injection is ignored. The heap
limit stays 256 MiB, and ArrayBuffer/external allocations are bounded only by
the address-space limit.

`profile-prep-backend` (real PipeWire, owned silent sink) was run with the
backend enabled: two consecutive runs held all six workload gates with zero
xrun gaps, zero stream drops, zero render overruns and the injected 100 ms
positive control still counting its gap. One earlier run of the same matrix
reported a single late voice reservation (2.7 ms past its deadline) in the
burst workload, with no xruns, drops or overruns; treat that as marginal jitter
to watch rather than a resolved result.

`profile-prep-backend` was also run with the whole engine pinned to one CPU
(`taskset -c 0`), both with and without the worker backend. Both runs held 3/4
of the reduced one-CPU workload matrix; the `burst` case had two late voice
reservations (2.676 ms) with no xrun gaps, stream drops or render overruns. The
worker is not the cause: the default backend produced the identical result.
Keep this as an honest one-CPU limitation, not a parity claim.

OOM behavior is covered by `test_oom` in `tests/test_js_worker.c`: a compile
request that tries to allocate a 1.5 GiB `ArrayBuffer` beyond the worker's
2 GiB address-space limit returns `RangeError: Array buffer allocation failed`
within the compile budget. The worker stays alive and reports the failure
instead of hanging or returning bogus metadata, so the engine can fall back to
its retained QuickJS evaluation.

The first host state is now forwarded instead of excluded: the engine sends its
confined startup prebake text in the compile frame, and the worker uses it as
`__compile`'s third argument. Previously a program with a startup prebake but no
`loadPrebake()` call silently dropped its compile-time defaults when delegated.
`test_prebake_transport` checks the raw frame (a `setDefault('gain', 1)` prebake
changes the worker's first event) and `test_prebake_engine` checks that such a
program delegates and matches its QuickJS twin byte-for-byte. Forwarding the
prebake exposed a deeper mismatch: the shared worker VM kept globals across
compiles, so `preCount` went 1 → 2 where QuickJS creates a fresh context per
evaluation. The worker now rebuilds its `node:vm` context on every compile;
`tests/test_javascript.c --prebake` covers the sequence. Named `loadPrebake()`
calls are now forwarded too: the engine records each confined prebake name and
text it loaded during evaluation and sends a JSON table in the compile frame.
The worker's `__hostPrebake` reads only from that table, closes it after
evaluation like the QuickJS host, and never touches the filesystem; a missing
name is still an error. `test_named_prebake_engine` checks delegation and
event parity for a note driven by a prebake global.

Live slider values are forwarded too. The engine records the host's bind
indices during evaluation and sends them with current values at compile; each
query frame carries the latest values, and the worker's `__hostSliderBind`
returns the same host index so source-position binds keep their identity. Slider
programs now delegate, and `test_slider_delegation` checks that a
`slider_set(0, .9)` between queries reaches the worker and matches a QuickJS
twin.

`midin` CC reads are lazy (a pattern can read any CC at query time), so the
compile stage cannot know which ones to forward. Each query frame now carries
all nonzero CC values (bounded at 128 × 16 entries) as a bounded map in the
worker; `__hostMidiOpen` succeeds there and `test_midi_delegation` checks that a
live CC change reaches the worker and matches a QuickJS twin. `midikeys`
consumes a host event queue and still stays on QuickJS.

Csound programs now delegate too: the worker's `__hostCsound` is a bounded
no-op (16 loads, the same kind/text size checks as the host), while the engine
still applies the real orchestra after evaluation through its confined renderer.
`test_csound_delegation` covers both `.csound()` and `loadCsound()` against a
QuickJS twin; query events do not depend on engine Csound state.

Visual programs delegate as well. Visual sites are still configured by
`configure_visuals()` in QuickJS, but `scopeMask` values arrive on musical
events from the worker's shared bundle; `test_visual_delegation` checks that a
`._scope()` program delegates with masked events identical to QuickJS.

With that, all formerly excluded callback categories have worker paths;
`midikeys` is the one remaining QuickJS-only reader because it consumes a host
event queue. The remaining work is the dependency-free standalone V8 embedder
build and the decision to make any backend other than QuickJS the default.

Failed-evaluation retention is cross-checked: a rejected live evaluation leaves
the shared worker bound to the last playable program, which keeps delegating and
returns byte-identical events (`test_engine_retention` in
`tests/test_js_worker.c`).

### Native embedded V8 worker prototype (2026-11-20)

`src/v8_worker.cc` implements the same framed protocol in C++ against the
pinned V8 16.0.94 static library built by the AUR recipe in
`packaging/aur/libv8/`. CMake `-DWITH_V8=ON` builds `tompouce-v8-worker` from a
`v8` pkg-config package (default `OFF`); select it with
`TOMPOUCE_QUERY_BACKEND=native` and `TOMPOUCE_V8_WORKER=/path/to/binary`.
QuickJS remains the default and the Node worker remains valid. The native
worker needs `icudtl.dat` beside the binary and a 256 GiB `RLIMIT_AS` for V8's
virtual reservations (RSS is far smaller).

With the makepkg-built package used as an isolated prefix and no competing
build load, the compile-stage medians were 139/140 ms for IDs 13587/78179
(539 haps each) versus 128/129 ms for the Node worker. The full Bakery
comparison with the native worker matched **42/42** compositions, including
398/398 events on both heavy songs and 112/112 on ID 112818; no threshold or
baseline was changed. `test_js_worker` passes in native and Node modes, and a
focused ctest run (`js_worker|javascript|daemon|mcp_security|companion|audio_output`)
passed 6/6.

This is still an opt-in prototype, not a default-backend decision. The
acceptance gates above remain open: confinement across all V8 threads, JIT
mapping audit, OOM/malformed-frame/watchdog behavior at the embedder level,
cold-swap and low-CPU measurements, and audio continuity under GC/JIT/compile
load. The packaged V8 build also has its in-process sandbox disabled, and the
shim currently rebuilds the runtime bundle per evaluation, which is worth
measuring before any default switch.

## Packaging and notices

`install.sh` copies `scripts/query-worker.mjs` alongside the installed binary
(`RUNTIME_FILES` in `scripts/install.py`), and `tests/test_install.py` covers that
out-of-tree copy. No JavaScript bundle or Node runtime is installed for the
worker: the engine sends its own committed, embedded bundle in the init frame,
and the worker evaluates it through `node:vm`. `NOTICE` and the README document
that Node.js is an optional runtime dependency (MIT-licensed, with V8 under
BSD-3-Clause) whose notices come with the user's Node installation. The worker
resolves its script relative to the installed executable, so the symlinked
launcher in `~/.local/bin` finds the script under `~/.local/lib/tompouce`.

Still required before this can be a supported or default backend: host callbacks
for the excluded features (Csound loads, MIDI snapshots, live sliders, prebakes)
and OOM/cold/one-CPU behavior. The Node process also adds a runtime Node
dependency and ~113–158 MiB RSS, which is why it is opt-in.

## Build requirement

No standalone V8 is installed on this machine: `nodejs` ships a static binary
with V8 14.6, there is no `libnode.so` or `libv8`, and Arch provides no V8
package. A production worker therefore needs a from-source embedder build
(depot_tools, `gclient sync`, `gn`/`ninja`, roughly several GiB of checkout and
a long build) pinned to an exact V8 revision and packaged with its notices,
ICU data and snapshot. The host has 243 GiB free, 31 GiB RAM and 16 CPUs, so the
build is feasible; it is the first real cost of this migration. An isolated
probe build is recorded below, but it is not a production backend.

The AUR [`v8-r`](https://aur.archlinux.org/packages/v8-r) package is a possible
developer shortcut: it pins V8 14.5.137, performs that same depot_tools build
with system ICU and clang/lld, and installs shared `libv8`, `libv8_libbase` and
`libv8_libplatform` libraries, headers and pkg-config files (plus Abseil, zlib
and partition_alloc component libraries). It is usable for prototyping the C++
shim, but not as the project's dependency: it is AUR-only, was flagged
out-of-date in January 2026, its source repository was archived and its
maintainer announced they are stepping away, and the component build adds
notices that must be audited. Arch's official repositories currently provide
only Node.js, not a standalone `v8`. Installing an AUR package changes system
state and is not done as a test here; the release build still needs a V8
revision pinned and owned by this project.

### Pinned V8 16.0.94 build probe (2026-11-20)

An isolated probe built the latest stable V8 tag (`16.0.94`,
`3d4477abfde92d54e495b34b7fa14f585e5c476e`) with depot_tools pinned at
`e923de63bfc631d5a09ef5324635d008d50dc0ca`, using the host's clang 23, lld
and ninja, with no V8 system package installed. Measured here: fetch plus
dependency sync took about three minutes (9.6 GiB V8 checkout, 1.2 GiB
depot_tools); the `v8_monolith` build took 1406 s (23.4 min) and produced a
131 MB `libv8_monolith.a` in a 913 MB output tree. Linking a minimal embedder
also needs 47 Rust/ICU rlibs (~150 MB), an 11 MB `icudtl.dat`, `-latomic` and
`lld` (V8 16 emits experimental CREL relocations); the resulting
`hello-world` binary is 67 MB and ran successfully. A plain `ld` link with
just `libv8_monolith.a` fails.

This is a feasibility probe, not a production recipe. It disabled V8's
in-process sandbox (the pinned default requires hardened libc++), disabled
Chromium-specific clang plugins, pinned `clang_version = "23"`, and needed a
compatibility symlink from `x86_64-unknown-linux-gnu` to Arch's
`x86_64-pc-linux-gnu` clang resource directory. It shows a pinned, self-owned
V8 embedder build is feasible on this host; it does not change the default
backend or satisfy the acceptance gates above.

The worker must link V8 as a single-threaded embedder: a minimal
`v8::Platform`, one isolate entered by one thread, `TerminateExecution` driven
by a parent-side watchdog, a heap limit plus separate external-memory accounting,
and OS confinement applied to every thread V8 creates (compiler and GC workers
included). None of that is exercised by the Node diagnostic.
