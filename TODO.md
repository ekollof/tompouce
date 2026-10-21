# TODO

Active work only. Completed work and measurement details are preserved in the
[preparation/parity archive](docs/archive/preparation-parity-roadmap.md) and
[earlier history](docs/archive/todo-history.md). Current behavior and coverage
live in [JavaScript/native audio](docs/javascript.md),
[audio comparisons](docs/audio-comparison.md) and
[architecture](docs/architecture.md). Follow [AGENTS.md](AGENTS.md) for security,
verification, installation and upstream terminal constraints.

## Local sample banks with Strudel manifests

Import a local bank containing `strudel.json`, preserving sound names, array
order and pitch regions instead of treating it as an alphabetical audio folder.
Public URL/GitHub manifests already work through `samples()`; this feature adds
persistent offline imports through the existing sound store.

- [ ] Require compatibility with Strudel's **import-sounds** workflow. Inspect
      its actual implementation and test the same bank fixtures through both
      importers before finalizing the contract. Preserve compatible folder and
      manifest layouts, sound names, variant ordering and resulting pattern
      usage without requiring Tompouce-specific bank edits. Resolve conflicting
      assumptions in the provisional steps below against that behavior; document
      unavoidable security/platform restrictions and test them explicitly.
- [ ] Define supported manifest forms against the existing loader and actual
      Strudel behavior: arrays, single-file entries, pitch-keyed maps, nested
      directories and `_base`. Report unsupported forms explicitly. Preserve
      precedence: explicit `samples()` maps, imported sounds, then defaults.
- [ ] Detect `strudel.json` at the selected root in the shared F6, companion and
      MCP import service. Keep existing audio-folder behavior when no manifest
      exists; invalid manifests must fail rather than silently fall back.
      Recursive discovery of multiple banks is outside the initial scope.
- [ ] Resolve referenced audio through confined filesystem operations inside
      the selected root. Support relative subdirectories and a confined local
      `_base`; reject absolute paths, traversal, symlinks, special files and
      remote references in local-import mode. Preserve public-fetch SSRF checks
      and the restriction against filesystem paths in `samples()`.
- [ ] Persist normalized mappings and referenced audio in the owned sound store,
      retaining variant order, pitch metadata and shared-file references across
      restart. Version the schema if needed and retain existing imports. Reuse
      audio/import limits; also bound manifest bytes, nesting, entries, path
      lengths and total work.
- [ ] Validate and stage the complete bank before publication. Missing or
      unsupported audio, invalid mappings, quota failures and interrupted writes
      must preserve the previous bank and clean up staging. Define bank identity,
      collision/replacement policy and obsolete-file cleanup. Reimports must
      preserve queued notes and active voices.
- [ ] Expose manifest banks consistently in F6 and companion with correct variant
      counts and source snippets. Use existing UI/MCP diagnostics for failures;
      avoid unnecessary disclosure of local paths to browser clients.
- [ ] Add owned fixtures for non-alphabetical variant order, nearest-pitch
      selection, `_base`, nested/shared files, precedence, restart persistence,
      reimport and legacy folder imports. Independently compare supported map
      interpretation with Strudel; native-only tests do not prove web parity.
- [ ] Test rejection and absence of side effects for traversal, escaping symlinks,
      malformed/deep/oversized JSON, URLs, missing/unsupported assets, collisions,
      quota exhaustion and partial writes. Verify all import frontends share
      validation. Run security tests/sanitizers and relevant catalog/import/audio
      checks with owned fixtures and isolated resources.
- [ ] Document supported manifests, `_base`, offline use, limits and replacement
      behavior. Sync edited wiki pages per AGENTS.md. Install verified runtime
      changes and check the installed import flow without touching the user's
      running session.

## CPU optimization follow-up

Convolution now selects AVX2, AVX or SSE2 accumulation at initialization
according to CPU/OS support, with portable C fallback and `TOMPOUCE_SIMD`
overrides. The [benchmark and validation report](docs/simd-benchmark.md) records
a 1.84x full three-second convolution speedup on a Ryzen 5800X3D, exact C/AVX2
PCM comparisons, sanitizers, web gates and selected real-backend workloads.

- [ ] Broaden simultaneous long-convolver scaling measurements beyond the
      selected orbit/preparation workloads. Preserve real-backend deadline
      checks, audio parity, portable fallback and per-kernel targeting; do not
      enable global AVX2 or relaxed math flags based on microbenchmarks.
- [ ] Profile per-voice frequency/FM work and inactive orbit room/delay overhead
      for portable fast paths that retain tails and automation. Stretch
      preparation and FFTs are secondary candidates; AVX2 did not beat portable
      O3 for the looping-sample callback cases.

## Remaining parity work (optional)

Use independent web comparisons and explicit acceptance criteria for each gap.
Preserve thresholds, historical fixtures and execution budgets; completed gates
and measured divergences are recorded in the archive.

- [ ] Broaden Csound loader failure-side-effect coverage beyond URL caching,
      parked engines and console diagnostics.
- [ ] Extend MIDI output coverage for string CC names, MIDI maps, NRPN,
      `isController`/options-object defaults, duplicate scalar/array sends,
      note-off timestamps/durations and pinned numeric-string coercion drift.
- [ ] Evaluate input beyond native CC/note-on. Web per-device stores versus
      native merged stores and web non-CC store pollution are documented
      divergences; do not break merged semantics or reproduce pollution merely
      to turn a comparison green.

Scope limits: generated multi-room tails use different random streams; browser
wall-clock duck timers under contention have no exact native audio-clock
equivalent. Slider/expression changes retain event-level coverage where no
bare-REPL counterpart exists. Full-song waveform parity, complete decoder/synth
equivalence, remote-download deadlines, Hydra/browser input-device APIs and
arbitrary npm resolution remain non-goals unless explicitly re-scoped.
Non-PipeWire backend underruns remain unmeasured.

## Terminal blockers awaiting upstream

Large paste flood/drop and split-UTF-8 hangs await Notcurses
[#2948](https://github.com/dankamongmen/notcurses/issues/2948), related to
[#2548](https://github.com/dankamongmen/notcurses/issues/2548), and
[#2947](https://github.com/dankamongmen/notcurses/issues/2947).
Retain system Notcurses and its protocol ownership; do not add local fixes while
upstream fixes are pending. Keep `tests/test_paste_stress.py` reproductions
separate from maintained acceptance cases and report failures honestly.
