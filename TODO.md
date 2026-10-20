# TODO.md

Active work only. Completed checklists, old handoffs and the retired parser
roadmap are preserved in [the historical archive](docs/archive/todo-history.md).
Current behavior and coverage live in [JavaScript/native audio](docs/javascript.md),
[audio comparisons](docs/audio-comparison.md) and [architecture](docs/architecture.md).
Follow [AGENTS.md](AGENTS.md) for implementation, verification and installation.

Keep the source buffer plain text, with live widgets beside the code. The
supported language direction is upstream JavaScript Strudel.

Terminal follow-up: large ASCII/Unicode paste loss/hangs and UTF-8
codepoints split between reads in the Notcurses integration await upstream
fixes, retaining the system library and its protocol ownership. `tests/test_paste_stress.py` keeps
these failing reproductions separate from the maintained acceptance cases.
The 100 ms inter-chunk cancellation bug is fixed; delayed bodies, delayed
closing markers and shutdown of an unfinished paste have owned PTY checks.
Upstream: flood drops are dankamongmen/notcurses#2548 (still reproducing on
3.0.17, filed as #2948); a split multibyte wedging the input thread is filed
as #2947. A lost closing marker abandons the paste after two seconds without
progress instead of hanging, and Ctrl-Q quits even mid-paste.

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

Normal generator publication now publishes each completed hap through its own
append-mode event-queue transaction. Haps are classified first, source jobs are
submitted to a bounded pool, and commits stay in scheduling order, so ducks,
delay and room changes, equal-time notes and generation checks still happen in
hap order. An earlier ready note becomes visible before a later slow source
finishes; it is not held until the whole query window completes. Obsolete
generation results are discarded before publication. Accepted duck controls
publish before the source wait, including when the source later fails or the
program changes; an owned blocked-source callback regression and actual web
Superdough ordering check cover this prefix. Audio already committed is
never withdrawn. The refresh transaction still replaces queued future events for
live slider tests and now also filters replacement onsets against the final
cutoff after preparation, so an onset that becomes due during preparation is
not reinserted.

The pool defaults to at most four workers, bounded by the CPU budget derived
from process affinity and visible ancestor cgroup v1/v2 quotas
(`TOMPOUCE_PREP_WORKERS=1..8` overrides it). Its general queue is
`workers*32`, capped at 512, and the staging
queue keeps its existing 8192-event bound. Sample/native/stretch preparation
and custom-IR kernel FFTs use the general workers. Generated rooms use a
32-request serial FIFO lane in the same pool so their order does not change;
the serial lane adds one thread beyond the general workers. A room completion
holds the generation guard across its generation check and convolver
publication, so a program transition cannot make the check stale. Room kernels
still publish on that serial lane rather than the event-queue transaction;
ordering against an intervening custom IR and failure side effects is covered
by the step 5 comparison. Pool startup failure falls back to inline source/kernel
preparation and is reported on the console; room requests report a room failure
instead.

On the step 1 workload, four distinct stretches took about 29.4 ms with one
worker, 15.2 ms with two and 9.3 ms with four. `test_prep_profile` reports the
workers, total pool threads and process CPU budget, and records that
worker-count comparison. `test_live_sliders` blocks a later source and verifies
that an earlier ready note reaches the queue, and advances the owned clock
across a staged refresh onset. `test_room` holds the generation guard while a
transition thread waits. Generated-room failures, queue overflow and
startup/enqueue failures remain observable through the console and the room
failure counter.

Cgroup membership and mount roots are resolved from procfs, including nested
systemd scopes, alternate v1 CPU mounts and visible inherited limits. Namespace
ancestors that are not exposed by a mount cannot be inspected. Read-only fixture
tests cover these paths without changing host quotas.

## 4. Measure deadlines under the pool

Skip this step when the pool was not built.

- [x] Add producer/consumer preparation latency, publication, late-preparation
      and late-reservation counters, disabled by default and outside the audio
      callback. Measure paced cold, live-edit and long-source burst workloads
      with fresh caches and owned sample stores across CPU-bounded worker counts
      and 128/4,096-frame callbacks. `test_prep_deadlines` and
      `scripts/prep-deadline-qa.mjs` preserve timing violations in their reports;
      a structural CTest pass is not a timing gate pass.
- [ ] Settle observed deadline misses and measure real backend underruns under
      preparation load. Local fixture reads do not establish remote-download
      deadline behavior. Require existing event and audio parity gates to pass
      without relaxing execution budgets. Voice-pool burst checks reuse these
      counters. See [measurement details](docs/audio-comparison.md).

The isolated offline matrix covers 24 cases. Cold/live cases at both block
sizes hold fully with 1/2/4 workers; the sixteen-long-stretch burst sheds its
overload as specified skips (68/26/11 late sources at 128 frames on this
machine). The matrix gate requires zero late publications, zero late
reservations, zero callback overruns and zero host wake misses in every case;
voice-burst cases additionally require retiring graphs. A voice-burst workload
layers preparation-free sine voices with distinctly-stretched samples so the
128-voice pool saturates while workers carry real stretch load; its matrix
cases share the shed-clean gate and additionally require retiring graphs, so
the saturation proof cannot pass vacuously. Accepted onsets publish with zero
late publications and zero late reservations, shedding only specified skips.
The 4,096-frame block is
the output adapter's render-call cap, so both measured block sizes are
realizable. `profile-prep-backend` runs the same workloads through the real
PipeWire backend at 256-frame blocks into an owned null sink (routing verified
with `pw-dump`, defaults untouched): six cases show zero stream drops, zero
render overruns and no frame shortfall, with cold/live held, the long-stretch
burst shedding only skips, and the voice burst publishing all 209 sources at
peak 128 voices with 21 retiring graphs. `TOMPOUCE_OUTPUT_TARGET` selects that sink for owned
tests only. The maintained web/native audio gates and selected duck/event
comparisons pass without threshold or execution-budget changes (delay-automation
and live-change onset gates were added under steps 5–6). Remote-download
deadline behavior remains unmeasured.
Actual backend xrun measurement also remains open: `backend_drops` counts
sampled active-to-inactive stream transitions, not PipeWire underruns. The
null-sink checks establish stream continuity, rendered-frame coverage and
render-call deadlines, but cannot claim zero xruns. The report now labels
underruns unmeasured rather than inferring them from stream state.

## 5. Shared-effect comparisons

- [x] Broaden live shared-effect comparisons: delay automation PCM, overlapping
      generated-IR completion races and source-side effects of failed generation.
      Generated rooms now use a bounded worker queue, retain the old convolver
      until completion and preserve it on preparation failure. Deterministic
      pending, failure, tail and overflow checks and web buffer and
      parameter-guard checks cover this handoff; the single worker completes
      generated requests FIFO. Prove the same handoff under ordered publication
      when the pool exists. Voice retirement and live onset checks retain these
      tails, so settle them before those gates grow.

Four web/native PCM gates cover shared delay automation (feedback change, time
change, zero-time suspension and return to baseline) at 0.9999998+ correlation.
The return case exposed an offline-renderer skew: `render_audio` decided delay
automation at trigger time while the web guards on preparation-time values, so
a return matching the initial settings was scheduled natively but skipped by
the browser. The renderer now prepares delays upfront like the live commit
path; the scheduler path already decided at preparation time
(`test_delay_automation`). `test_generated_room_race` proves the handoff
through the pool and event-queue transaction: a decoy request serializes the
lane, the intervening custom IR publication is observed first, and the older
generated completion replaces it; an oversized room fails deterministically
while its note still commits, the voice renders on the previous convolver, and
the retained equality guards suppress a repeat request. The full matrix passes
38/38 with the diagnostic wet Arpoon unchanged.

## 6. Fractional onsets under live changes

- [x] Extend verified fractional sample onsets to live source, tempo, transport
      and shared-effect changes against actual web output. Steady playback is
      already gated; this is the live-change delta, written against ordered
      publication and the shared-effect timing from the previous step.
      Event-value and clock gates alone do not establish performance parity.

Pattern-expressed changes gate through the static matrix: stretch-variant
switches, shared delay automation and (as a diagnostic) generated-room
switches with fractional onsets. The room-switch onsets align to the exact
sample; its wet tails differ because the web regenerates rooms with fresh
randomness per call while native uses a continuous seeded stream and caches. Static tempo
gates proved unexpressible: the browser offline render ignores event-level
cps pivots for onset mapping, so renderer-side threading was reverted.
Engine operations gate through the deterministic live harness
(`test-live-reference-web`): source re-evaluation, pattern-level tempo
pivots, external tempo pivots and transport Stop/restart with fractional
onsets all observe
correlation
1.0 across repeated runs, with equal voice/hap counts on tempo. External
tempo anchoring was assumed divergent until a harness tick-cadence fix
showed both sides anchor at the scheduling frontier with no product change
needed. A mock interval that stays dead after Stop was required to
avoid re-firing scheduled haps, matching production interval cleanup; mock
time still advances through stops, matching the production audio clock, so
post-restart catch-up windows are not skipped as too late.

## 7. Voice-pool eviction, then bursts

- [x] Extend web voice-pool eviction coverage and verify backend bursts under
      scheduling and preparation load without dropping accepted onsets or
      controls. FIFO and ordinary synth and sample gain ramps now retain
      bounded retiring graphs through source stop at incoming target + 250 ms,
      retaining downstream effect tails until original cleanup. Explicit rests
      and failed sources now remove a full pool's oldest entry without
      reserving a replacement. A 62-case web/native gate covers short and
      coincident ADSR, same-group sample cuts and selected filter ring-out:
      high-pass, band-pass, phaser and coarse tails gate post-stop PCM at
      3e-5, and native cut ordering (group isolation, first-cutter-wins,
      equal-time onset-serial order, oscillator exclusion) is locked in.
      Stolen drums fade by the same retirement envelope (the deployed pool
      ramps every evicted entry regardless of type), proven by an exact
      retired-vs-unevicted A/B and a mixed-type saturation burst. A
      voice-burst deadline workload proves saturated bursts with the step 4
      counters, offline and through the real-backend null-sink run. The 128
      retiring-graph bound reports overflow and replaces the graph with the
      earliest stop deadline. Remaining gaps are documented diagnostics, not
      gates: chorus has no deployed web control to compare against (a
      native-only impulse regression locks its delay line instead), and drum
      timbre parity falls under the synthesis-equivalence non-goal below.
      Vowel tails gate through a 15-vowel web impulse-response comparison
      against the deployed five-formant filter.

## 8. Duck coverage

- [x] Broaden duck coercion and live callback-load coverage beyond the selected
      web comparisons. A 129-case web/native gate now covers numeric string
      forms, nested onsets, past endpoints, late callbacks and cancellation of
      rising and falling ramps, including failures after gain reanchoring.
      An 8,944-case exact gate covers numeric concatenation across binary64
      boundaries, long tails, radix forms and the full Unicode
      whitespace/confusable set in every suffix position, which aligns (not
      removes) the bounded summary path for long onset strings. Timer-creation
      failure keeps existing automation, voices and later haps intact with a
      distinct diagnostic on both sides.
      Interrupted ramps now anchor to the last rendered sample, matching
      AudioParam.value. A native 150-note duck flood proves accepted callbacks
      survive pool saturation (exact per-orbit automation, ordered mailbox,
      bit-identical blocks), and the voice-burst deadline workload carries
      duck controls under preparation load with published-control counts in
      the report. Native timers dispatch per sample, so they are exact to the
      audio clock by construction; wall-clock fire-time parity with browser
      event-loop timers under host contention is not established and would
      not be meaningful where the clocks differ.

## 9. Csound loader side effects

- [x] Match Csound loader side effects before a later JavaScript failure, failed
      URL Promise caching and runtime worker diagnostics. Failed orchestra
      fetches stay rejected without refetching, matching the deployed
      loader's session promise cache (one observable fetch for two calls;
      oversize content still refetches). Loads that succeed before a later
      failure park their engine for the next evaluation, matching the
      deployed page engine. Engine runtime messages surface in the console
      ring over a dedicated worker socket with an omission summary under
      floods, matching the deployed message channel; compile diagnostics
      keep their reply path. This is the confined compiler and
      renderer, not the preparation pool. Combined-output routing depends on
      what a failed load leaves active.

## 10. MIDI and combined output routing

- [x] Implement dominant MIDI and combined external-output routing against the
      web REPL. `.midi()` now selects dominant MIDI (ALSA note, no local
      voice) with channel from `midichan` defaulting to 1 and pitch from the
      resolved voice frequency; each external output (MIDI, OSC, Csound)
      fires independently while local synthesis stays off, and a failing
      Csound schedule still warns while MIDI goes out. Native `midichan`
      keeps its local-audio-plus-ALSA behavior. Rests and pitch-less drums
      carry no MIDI note. CC, program change, bend, touch, sysex and midicmd
      transport have deployed send coverage below; native port selection
      still broadcasts to subscribers. Preserve Csound and OSC
      isolation and verify ordering and effects when multiple output callbacks
      are combined. Note triggering, effect tails and Csound bypass are settled
      by the previous steps.

## 11. Controller input

- [ ] Verify MIDI and controller input, expression and live mapping changes
      against the actual web REPL rather than inferring behavior from control
      names. `midin` opens an ALSA sender and returns per-CC live readers
      (normalized 0..1, default 0, optional 1..16 channel); `midikeys`
      returns live keyboard notes with fixed durations, channel attribution
      and consume-once queries, rejecting a missing device like the web.
      Values merge across senders; input names accept client:port, numeric
      ids, substrings and first-sender default. CC persistence, device-only
      restore, reconnect, buffered input bursts and non-consuming visual
      reads have native regressions. Independent deployed-web input-message,
      persistence and reconnect comparisons remain open; the native ALSA
      loopbacks alone do not close web parity. Native stores currently merge
      senders, so device-isolated readers also need a comparison and correction.
      This only reads controls;
      doing it earlier would churn the live-control captures used above.

## Optional: full REPL parity

Not required for the ordered work above and never gate-blocking. If pursued,
close each gap with an independent web comparison and its own threshold;
a hosted-player drift is evidence to investigate, never a reason to loosen
a threshold. Steps 6–11 already scope the required slices; the remainder is:

- Live-change fractional onsets beyond the five gated step 6 scenarios.
- Voice-pool breadth is closed: chain order audited against the deployed
  per-source chain (positions match; static pan commutes with the phaser),
  chorus re-confirmed a no-op key on the deployed player, drum timbre stays
  under the synthesis-equivalence non-goal, and bursts stay covered by the
  existing saturation/shedding/determinism matrices.
- Duck wall-clock timer-fire parity under host contention (step 8 remainder;
  native dispatch is sample-accurate by construction; the web fires from a
  setTimeout 10 ms early and re-anchors late callbacks at its scheduling
  frontier, which no deterministic gate can reproduce). Coercion breadth is
  gated: per-target array mapping (59 web/native cases) plus null/boolean
  scalar coercion at ramp/PCM level.
- Csound loader failure side effects beyond the gated URL caching, parked
  engine and console diagnostics (step 9 remainder).
- Dominant MIDI output is gated beyond note triggering: CC (integer
  0..127 with 0..1 values, including `control()` aliasing), program
  change (truncated 0..127), pitch bend, channel aftertouch, framed sysex
  and transport `midicmd` all send through ALSA in web order with
  throw-abort parity, control-only haps send extras without notes, and
  `midiport` values broadcast. A 24-case web/native gate
  (`test-midi-reference-web`) covers hap values, commit decisions and actual
  deployed send bytes/order through an owned fake MIDI transport; native commit/delivery tests cover
  patterned fan-out, combined OSC/Csound routing and failing-Csound
  retention. Remaining gaps are documented non-gates: string CC names,
  MIDI maps, NRPN, `isController`/options-object defaults, duplicate
  scalar-plus-array sends, note-off timestamp/duration parity, and
  pinned-1.2.6 numeric-string coercion drift.
- Controller input is implemented, with independent web input gates still
  required by step 11: named senders persist CC state across restarts
  (numeric opens never persist), restore it to the device output on
  (re)connect, and resubscribe by client name after replugs, all proven
  over real ALSA ports including a delete/recreate cycle. Device-isolated
  readers and the web's handling of message types beyond native CC/note-on
  input remain part of the reopened comparison work.
- Transport restart after Stop is gated: both sides reset to cycle zero and
  re-fire early-cycle haps, with sample-exact onset agreement (correlation
  1.0). Stop itself (held tails finish, no new onsets) was already gated.
- External tempo-change pivot anchoring is gated: both sides anchor the
  pivot at the scheduling frontier (last queried end / next callback
  phase), with sample-exact onset agreement (correlation 1.0) through the
  deterministic live harness (`test-live-reference-web`, now five cases)
  plus a virtual-time event-onset gate. Pattern-level tempo pivots were
  already gated live; external pivots were covered at event-value level
  until the harness tick-cadence fix showed no product change was needed.
- Multi-room generated sequences stay diagnostic: single seeded rooms gate,
  but exact tail parity is unreachable (production randomness is unknowable:
  one chain-ID draw per trigger plus fresh draws, and the eager harness
  bounds per-hap capture). Native continues one LCG stream across
  generations so rooms vary per call deterministically; a CTest locks
  sequence variety and cache identity.

Standing non-goals unless explicitly re-scoped: full-song waveform parity,
complete decoder/synthesis equivalence, remote-download deadline behavior,
Hydra/browser input-device APIs and arbitrary npm module resolution.

The maintained audio matrix has 40 passing gates and two diagnostic cases (42
cases total), including eight independently
decoded downward-stretched MP3 piano cases, two shared-PCM stretch cases, four
fractional sample-onset cases, four shared delay-automation cases and two
live-change fractional-onset cases. Seeded generated rooms are gated; ordinary
wet Arpoon and multi-room generated sequences remain diagnostic. These checks
do not establish complete decoder, synthesis or full-song waveform parity.
Use owned test engines/browser profiles and isolated audio resources; leave the
user's running source, playback and routing untouched.
