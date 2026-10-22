# Native synth references

`src/native_synth.c` ports the ZzFX sample algorithm and implements bounded
operations for bytebeat and Kabelsalat. It does not execute source code or
JavaScript in the audio callback.

- ZzFX: Frank Force, MIT, <https://github.com/KilledByAPixel/ZzFX>.
  Retained license: `LICENSE-ZzFX`.
- Strudel/superdough 1.2.6 `zzfx.mjs`, `zzfx_fork.mjs`, `synth.mjs` and
  `worklets.mjs`: Strudel contributors, AGPL-3.0-or-later.
  Reference archive: <https://registry.npmjs.org/superdough/-/superdough-1.2.6.tgz>.
  Retained license: `LICENSE-Strudel`. The bytebeat presets in
  `js/native-synth.mjs` come from this version.
- Kabelsalat core 0.4.0 and lib 0.4.1: Felix Roos and contributors,
  AGPL-3.0-or-later, <https://github.com/felixroos/kabelsalat>.
  Retained license: `LICENSE-Kabelsalat`; package notices also travel in
  `js/dist/licenses.txt`.

The native node set and resource bounds are documented in `docs/javascript.md`.
The Strudel package versions are unchanged.

Pulse synthesis in `src/dsp.c` follows Superdough's `PulseOscillatorProcessor`
in `worklets.mjs`. Periodic coefficients/phase rotation and cached noise buffers
in `src/periodic.c` follow `synth.mjs` and `noise.mjs`, under the Strudel license
above. The Fourier table builder is an independent implementation of the
WebAudio periodic-wave normalization and frequency-band definitions.

Generated convolution rooms follow `superdough` 1.2.6 `reverbGen.mjs`,
Copyright 2014 Alan deLespinasse, Apache-2.0 (`LICENSE-Apache-2.0`). Its
Strudel wrapper uses the license above. The native generator uses a fixed
LCG seed for reproducible rooms and retains the decay, fade and low-pass ramp.

Sample stretching in `src/stretch.c` follows the phase-vocoder and overlap-add
code in superdough 1.2.6 `worklets.mjs` and `ola-processor.js` (Strudel license
above), originally <https://github.com/olvb/phaze> (`LICENSE-Phaze`, Unlicense).
The real FFT layout follows Fedor Indutny's fft.js (2017, MIT, `LICENSE-FFT`);
the native inverse radix-2 FFT is an independent implementation.
