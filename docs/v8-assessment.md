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
