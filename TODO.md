# TODO.md

Active work only. Completed checklists, old handoffs and the retired parser
roadmap are preserved in [the historical archive](docs/archive/todo-history.md).
Current behavior and coverage live in [JavaScript/native audio](docs/javascript.md),
[audio comparisons](docs/audio-comparison.md) and [architecture](docs/architecture.md).
Follow [AGENTS.md](AGENTS.md) for implementation, verification and installation.

Keep the source buffer plain text, with live widgets beside the code. The
supported language direction is upstream JavaScript Strudel.

## Performance and parallel preparation

- [ ] Profile CPU time, wall time and lock contention in sample fetching/decoding,
      stretch preparation and impulse generation/FFT preparation. Measure cold
      caches, live edits and bursts before choosing worker counts.
- [ ] Move expensive stretch and convolution preparation outside cache locks.
      Keep lookup/publication locks short; preserve bounded caches, queued/voice
      references and safe handling of duplicate jobs for the same cache key.
- [ ] Add a bounded worker pool for independent sample, stretch and impulse
      preparation jobs. Bound queued work and memory, make failures observable,
      and benchmark worker counts against the CPUs available to the process.
- [ ] Publish completed preparation in scheduling order, preserving shared-effect
      changes, source/generation identity and accepted notes/controls. Discard
      obsolete unfinished work only when it has not already been committed.
- [ ] Measure preparation latency, scheduling deadlines, missed onsets and
      underruns across worker counts and heavy workloads. Require the existing
      event/audio parity gates to pass without relaxing execution budgets.

Keep QuickJS serialized and UI work on the foreground thread. The audio callback
must never wait for preparation workers or perform allocation, decoding, fetching,
JavaScript, FFT preparation or blocking syscalls. Parallel voice rendering needs
a separate measured design; it is not part of the initial preparation pool.

## Live-performance and audio parity

- [ ] Extend verified fractional sample onsets to live source, tempo, transport
      and shared-effect changes against actual web output. Event-value and
      clock gates alone do not establish performance parity.
- [ ] Finish web voice-pool eviction fades and verify backend bursts under
      scheduling/preparation load without dropping accepted onsets or controls.
      Saturated native allocation now uses verified insertion FIFO; eviction
      still cuts immediately. The web ramps/stops at incoming target + 250 ms,
      and even rests evict before source selection. Preserve retiring graphs
      with bounded resources and verify synth/sample envelope differences.
      The prepared-event callback harness covers 64 notes plus 64 independent
      duck callbacks at 1–8,192 frames; it does not prove overload/underrun safety.
- [ ] Finish generated-IR asynchronous replacement and preparation-failure
      behavior; broaden live delay-automation PCM comparisons.
- [ ] Finish remaining invalid/non-finite duck coercions, JavaScript numeric
      string forms and onset addition semantics, including cancellation after
      the callback has reanchored its gain.
- [ ] Remove or align the 127-byte normalized duck onset-string limit and verify
      remaining exotic string forms and late-callback behavior.
- [ ] Match Csound loader side effects before a later JavaScript failure, failed
      URL Promise caching and runtime worker diagnostics.

The maintained audio matrix has 34 passing gates, including eight independently
decoded downward-stretched MP3 piano cases, two shared-PCM stretch cases and four
fractional sample-onset cases. Seeded generated rooms are gated; ordinary wet
Arpoon remains diagnostic. These checks
do not establish complete decoder, synthesis or full-song waveform parity.
Use owned test engines/browser profiles and isolated audio resources; leave the
user's running source, playback and routing untouched.

## External outputs and controllers

- [ ] Implement dominant MIDI and combined external-output routing against the
      web REPL. Native `midichan` currently keeps local audio plus ALSA output;
      the web's `.midi()` bypasses local audio. Preserve Csound/OSC isolation and
      verify ordering and effects when multiple output callbacks are combined.
- [ ] Verify MIDI/controller input, expression and live mapping changes against
      the actual web REPL rather than inferring behavior from control names.

## Legacy dialect migration

- [ ] Migrate bundled examples, editor helpers, tests and other dependencies
      before removing the historical C statement/mini parser and line dialect.
      Keep existing legacy users working until they have a migration path.
      Do not expand the retired parser or implement its archived roadmap.
