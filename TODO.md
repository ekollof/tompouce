# TODO.md

Active work only. Completed checklists, old handoffs and the retired parser
roadmap are preserved in [the historical archive](docs/archive/todo-history.md).
Current behavior and coverage live in [JavaScript/native audio](docs/javascript.md),
[audio comparisons](docs/audio-comparison.md) and [architecture](docs/architecture.md).
Follow [AGENTS.md](AGENTS.md) for implementation, verification and installation.

Keep the source buffer plain text, with live widgets beside the code. The
supported language direction is upstream JavaScript Strudel.

Work proceeds in the order below. Later items reuse the workloads, publication
rules and comparison gates from earlier ones. Do not add new audio baselines
until the preparation path they measure has settled.

Keep QuickJS serialized and UI work on the foreground thread. The audio callback
must never wait for preparation workers or perform allocation, decoding, fetching,
JavaScript, FFT preparation or blocking syscalls. Parallel voice rendering needs
a separate measured design; it is not part of the initial preparation pool.

## 1. Profile preparation

- [x] Profile CPU time, wall time and lock contention in sample fetching/decoding,
      stretch preparation and impulse generation/FFT preparation. Measure cold
      caches, live edits and bursts before choosing worker counts. Keep these
      three workloads; deadline measurement repeats them.
      `tests/test_prep_profile.c` is that run.

Decode runs outside the sample lock, including when several files are decoded
at once. Stretch overlap-add, sample-convolution FFTs and generated-IR
synthesis hold their cache mutexes for essentially the whole compute. A
repeated key does not redo that work. A live parameter change misses
preparation and does not decode the sample again. Several stretches serialize
on the stretch lock. Stretch and generated IR use different locks and overlap.
This run had no installed MP3 bank. Release the stretch, convolution and
reverb computes from those locks before choosing a worker count.

## 2. Release stretch and convolution cache locks

- [x] Move expensive stretch and convolution preparation outside cache locks.
      Keep lookup and publication locks short. Preserve bounded caches,
      queued and voice references, and safe handling of duplicate jobs for the
      same cache key. The existing event and audio gates are the regression
      net; do not add baselines here.
- [x] Rerun the step 1 workloads after the locks are released. If scheduling
      deadlines already hold, skip the publication contract and the worker
      pool and continue with shared-effect comparisons.

Stretch overlap-add, sample-impulse construction and generated-IR synthesis/FFTs
now run outside their cache locks. In-flight reservations count toward the cache
byte bound; concurrent jobs for one key publish a single entry and free the
losing copies. On the step 1 workload, a cold stretch held the lock for about
61 µs around 12.6 ms of work. Four distinct stretches summed about 38.7 ms of
work but finished in about 10.0 ms of wall time with about 12 µs of wait.
Convolution and generated-IR lock holds stayed in the low microseconds. The MP3
profile branch now runs when the bank is installed. These are one machine's
timings, not a parity gate.

## 3. Publication contract, then the worker pool

- [x] Publish completed preparation in scheduling order, preserving shared-effect
      changes, source and generation identity, and accepted notes and controls.
      Discard obsolete unfinished work only when it has not already been
      committed. Generalize the staged event-queue refresh so a late completion
      uses that transaction. Fold the generated-room queue into the same
      completion rules instead of adding a second path.
- [x] Add a bounded worker pool for independent sample, stretch and impulse
      preparation jobs, committing through that transaction. Bound queued work
      and memory, make failures observable, and benchmark worker counts against
      the CPUs available to the process.

Normal generator publication now uses an append-mode event-queue transaction.
Haps are classified first, source jobs are submitted to a bounded pool, and the
generator waits for and commits them in scheduling order, so ducks, delay and
room changes, equal-time notes and generation checks still happen in hap order.
Obsolete generation results are discarded before publication; audio already
committed is never withdrawn. The existing refresh transaction still replaces
queued future events for live slider tests.

The pool defaults to at most four workers, bounded by the online CPU count
(`TOMPOUCE_PREP_WORKERS=1..8` overrides it). Its general queue is
`workers*32`, capped at 512, and the staging queue keeps its existing 8192-event
bound. Sample/native/stretch preparation and custom-IR kernel FFTs use the
general workers. Generated rooms use a 32-request serial FIFO lane in the same
pool so their order does not change; completed rooms carry the scheduling
generation and a stale completion is not published. Pool startup failure falls
back to inline source/kernel preparation and is reported on the console; room
requests report a room failure instead.

On the step 1 workload, four distinct stretches took about 29.4 ms with one
worker, 15.2 ms with two and 9.3 ms with four. `test_prep_profile` now records
that worker-count comparison. Generated-room failures, queue overflow and
startup/enqueue failures remain observable through the console and the room
failure counter.

## 4. Measure deadlines under the pool

Skip this step when the pool was not built.

- [ ] Measure preparation latency, scheduling deadlines, missed onsets and
      underruns on the step 1 workloads, across worker counts and heavy
      workloads. Require the existing event and audio parity gates to pass
      without relaxing execution budgets. Voice-pool burst checks reuse these
      counters.

## 5. Shared-effect comparisons

- [ ] Broaden live shared-effect comparisons: delay automation PCM, overlapping
      generated-IR completion races and source-side effects of failed generation.
      Generated rooms now use a bounded worker queue, retain the old convolver
      until completion and preserve it on preparation failure. Deterministic
      pending, failure, tail and overflow checks and web buffer and
      parameter-guard checks cover this handoff; the single worker completes
      generated requests FIFO. Prove the same handoff under ordered publication
      when the pool exists. Voice retirement and live onset checks retain these
      tails, so settle them before those gates grow.

## 6. Fractional onsets under live changes

- [ ] Extend verified fractional sample onsets to live source, tempo, transport
      and shared-effect changes against actual web output. Steady playback is
      already gated; this is the live-change delta, written against ordered
      publication and the shared-effect timing from the previous step.
      Event-value and clock gates alone do not establish performance parity.

## 7. Voice-pool eviction, then bursts

- [ ] Extend web voice-pool eviction coverage and verify backend bursts under
      scheduling and preparation load without dropping accepted onsets or
      controls. FIFO and ordinary synth and sample gain ramps now retain
      bounded retiring graphs through source stop at incoming target + 250 ms,
      retaining downstream effect tails until original cleanup. Explicit rests
      and failed sources now remove a full pool's oldest entry without
      reserving a replacement. A 54-case web/native gate covers short and
      coincident ADSR, same-group sample cuts and selected filter ring-out.
      Broaden cut ordering, other per-voice effect graphs and the remaining
      native source types against actual web PCM, using the tails settled
      above. The 128 retiring-graph bound reports overflow and replaces the
      graph with the earliest stop deadline. Prepared saturation checks
      preserve shared delay tails and independent duck callbacks across
      1–8,192-frame bursts; they do not prove producer scheduling or underrun
      safety under preparation load. Run that load proof with the deadline
      counters from step 4 when the pool exists.

## 8. Duck coverage

- [ ] Broaden duck coercion and live callback-load coverage beyond the selected
      web comparisons. A 129-case web/native gate now covers numeric string
      forms, nested onsets, past endpoints, late callbacks and cancellation of
      rising and falling ramps, including failures after gain reanchoring.
      Interrupted ramps now anchor to the last rendered sample, matching
      AudioParam.value. Nothing later depends on more duck cases; do this once
      publication is stable so late-callback cases are not rewritten.

## 9. Csound loader side effects

- [ ] Match Csound loader side effects before a later JavaScript failure, failed
      URL Promise caching and runtime worker diagnostics. This is the confined
      compiler and renderer, not the preparation pool. Combined-output routing
      depends on what a failed load leaves active.

## 10. MIDI and combined output routing

- [ ] Implement dominant MIDI and combined external-output routing against the
      web REPL. Native `midichan` currently keeps local audio plus ALSA output;
      the web's `.midi()` bypasses local audio. Preserve Csound and OSC
      isolation and verify ordering and effects when multiple output callbacks
      are combined. Note triggering, effect tails and Csound bypass are settled
      by the previous steps.

## 11. Controller input

- [ ] Verify MIDI and controller input, expression and live mapping changes
      against the actual web REPL rather than inferring behavior from control
      names. This only reads controls; doing it earlier would churn the
      live-control captures used above.

The maintained audio matrix has 34 passing gates, including eight independently
decoded downward-stretched MP3 piano cases, two shared-PCM stretch cases and four
fractional sample-onset cases. Seeded generated rooms are gated; ordinary wet
Arpoon remains diagnostic. These checks
do not establish complete decoder, synthesis or full-song waveform parity.
Use owned test engines/browser profiles and isolated audio resources; leave the
user's running source, playback and routing untouched.
