# Parity bounds

Tompouce targets the deployed Strudel web REPL, but a few behaviors are
deliberately native-specific or cannot be measured on this machine. This file
is the consolidated ledger for those bounds. "No unchecked TODO items" means
there is no known implementation task left; it does **not** mean every browser
byte stream is sample-identical.

Each row names the gate or measurement that keeps the bound from drifting.

| Area | Web behavior | Native behavior | Gate / measured bound | Status |
| --- | --- | --- | --- | --- |
| Default MIDI destination | Picks the IAC-named WebMIDI output, else the first output. | Picks the IAC-named writable output, else the first writable output, then the first subscriber if no output exists. Explicit names prefer subscribed destinations before writable outputs. | `test_midi` checks the IAC/first-output default, explicit Midi-Bridge/PipeWire subscriber selection, and unsubscribed outputs. `managed_carla` passes with `.midi('PipeWire')`. | **Web-compatible default.** Explicit subscriber names are a native extension needed for PipeWire/`aconnect`; the no-selector path now follows the web output list. |
| Numeric `midiport` | Indexes the browser's WebMIDI output objects. | Indexes writable `MIDI_GENERIC` ALSA ports, including ports with no ALSA subscription. | `test_midi` computes a receiver's index from the same list and delivers by `#index`; a receiver with no subscription receives by name and index. | **Accepted mapping.** The list order is ALSA's, not the browser's; names are the portable selector. |
| MIDI input stores | Each device has isolated stores; every message byte is filed into the CC store. | Senders are merged; CC, note and bend paths stay separate. | `scripts/midi-input-reference-qa.mjs` / `test-midi-input-reference-web`, plus `test_midi`. | **Accepted.** Matching per-device isolation would break the merged semantics native gates depend on. |
| Live `wtphaserand` | The wavetable worklet calls its own `Math.random()`. | A time/PID-seeded LCG initializes each voice phase. | Seeded `wt-sample-phaserand-*` cases gate exact draw ordering at `0xc0ffee`. `test-wtphaserand-randomness` compares 16 unseeded renders per side: latest RMS mean difference 0.185%, peak 2.11%; earlier samples 0.914%/1.355% and 0.591%/4.785%. Bound: 5% RMS mean, 10% peak mean, both sides vary, ranges overlap. | **Bounded statistical equivalence.** Unseeded live phases cannot be sample-locked; no sample-exact live PCM claim. |
| Physical DIN/USB output timing | WebMIDI device queue and hardware latency. | ALSA queue, PipeWire Midi-Bridge and the attached UMC404HD. | Not measured. `docs/plugin-hosting.md` records the setup and the missing hardware. | **Deferred by user.** No receiving synth or DIN OUT→IN loopback is available; do not substitute a virtual ALSA loopback. Host-side timing and queueing remain gated by `test_midi`. |
| Browser-only prebake helpers | `document`, `navigator`, `localStorage` and Superdough nodes are available. | Those globals stay undefined so `typeof` detection still works; a real use aborts setup with a named error. `bmod` and `FX` are supported. | `test_javascript` covers feature detection and both error paths; the starter's non-Volume `UISlider` calls warn and are skipped. | **Accepted ceiling.** |
| Browser visual styling/layout | Canvas contexts, custom fonts, backgrounds, spiral caps and pitchwheel labels. | Terminal equivalents where one exists; unsupported options are reported once and ignored. | `scripts/scope-reference-qa.mjs` and `test_terminal_graphics` cover the geometry that has a native counterpart. | **Accepted ceiling.** No browser-pixel parity is claimed. |
| Shared delay/reverb/duck tails and Csound output | Shared orbit effects and a combined Csound stream. | Same shared state, so a per-pattern tap cannot attribute those tails. | Visual and analyser tests; documented in `docs/javascript.md`. | **Accepted design.** Attribution would require duplicating shared state. |
| `bstab` in `roller2` | `getSound('bstab')` is empty in the deployed REPL. | Rejects only that hap with the sample-unavailable warning; `supersaw` and `white` still play. | `test_audio` gates one warning, one eviction plus two source events, and no `bstab` token. | **Resolved shared absence.** Not a web/native divergence. |
| Non-PipeWire backend underruns | Not applicable. | PipeWire is measured; other backends are not. | Recorded in `docs/audio-comparison.md`. | **Unmeasured environment.** Not a JavaScript-semantics divergence. |
| Managed plugin editor embedding | Not part of Strudel. | Hosted LV2/Carla plugins have no embedded editor. | `docs/plugin-hosting.md` documents the UI ceiling and its supported-class matrix. | **Accepted native ceiling.** |

## Closing the default-MIDI question

The no-selector default is now the web's IAC/first-output choice. PipeWire's
Midi-Bridge and explicit `aconnect` sessions are not writable outputs, so
`managed_carla` selects its route explicitly with `.midi('PipeWire')`.
Explicit string selectors search subscribed destinations before writable
outputs; numeric selectors and the default use the WebMIDI-style output list.
`test_midi` gates both the web default and the explicit subscriber path, and
`managed_carla` gates the end-to-end PipeWire route.

## Physical timing: deferred

The user has no receiving synth or DIN OUT→IN loopback available, so physical
DIN/USB timing is deferred rather than an open implementation task. Host-side
queueing, note-off deadlines, channel fan-out and MIDI message delivery remain
covered by `test_midi`; only the hardware/driver leg is unmeasured.

If hardware becomes available, connect either a receiving synthesizer to the
UMC404HD MIDI OUT or a 5-pin DIN cable from MIDI OUT to MIDI IN, then measure
note-on/off round-trip jitter and device response with the owned-session
discipline in `docs/plugin-hosting.md`. Do not substitute a virtual ALSA
loopback for that leg; it cannot observe hardware or driver queues.

## Verification shortcuts

```sh
./scripts/ctest-safe.sh --output-on-failure
./build/tests/test_midi
./build/tests/test_audio
cmake --build build --target test-midi-reference-web
cmake --build build --target test-wtphaserand-randomness
cmake --build build --target test-midi-input-reference-web
```
