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
- [ ] Extend web voice-pool eviction coverage and verify backend bursts under
      scheduling/preparation load without dropping accepted onsets or controls.
      FIFO and ordinary synth/sample gain ramps now retain bounded retiring
      graphs until incoming target + 250 ms. Check rest/missing-source eviction,
      cut-group interactions, short/coincident ADSR endpoints, per-voice effect
      tails after source stop and the remaining native source types against
      actual web PCM. The 128 retiring-graph bound
      reports overflow and replaces the graph with the earliest stop deadline.
      Prepared saturation checks preserve shared delay tails and independent
      duck callbacks across 1–8,192-frame bursts; they do not prove producer
      scheduling or underrun safety under preparation load.
- [ ] Broaden live shared-effect comparisons: delay automation PCM, overlapping
      generated-IR completion races and source-side effects of failed generation.
      Generated rooms now use a bounded worker queue, retain the old convolver
      until completion and preserve it on preparation failure. Deterministic
      pending/failure/tail/overflow checks and web buffer/parameter-guard checks
      cover this handoff; the single worker completes generated requests FIFO.
- [ ] Broaden duck coercion and live callback-load coverage beyond the selected
      web comparisons. A 129-case web/native gate now covers numeric string
      forms, nested onsets, past endpoints, late callbacks and cancellation of
      rising/falling ramps, including failures after gain reanchoring. Interrupted
      ramps now anchor to the last rendered sample, matching AudioParam.value.
- [ ] Match duck timer creation failures before gain cancellation, including
      source times below 10 ms (the web rejects a negative timer stop time).
      Verify source/output and repeated-target side effects at that stage.
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
