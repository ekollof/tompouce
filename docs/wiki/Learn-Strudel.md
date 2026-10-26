# Learn Strudel

Tompouce's 22-lesson Strudeltutor is a practical first stop: F2 `/tutor`, then F5 to apply edits and Ctrl+N/Ctrl+B to navigate. It uses built-in synths, so the core lessons work offline. See [Live coding and Strudeltutor](https://github.com/ekollof/tompouce/wiki/Live-Coding-and-Strudeltutor) for progress and save behavior.

## Official learning path

Work through these official Strudel pages in order. They include browser examples you can edit and hear:

1. [Getting started](https://strudel.cc/workshop/getting-started/) — introduction to live-coded patterns.
2. [First sounds](https://strudel.cc/workshop/first-sounds/) — rhythms, samples and mini-notation basics.
3. [First notes](https://strudel.cc/workshop/first-notes/) — pitch, scales and concurrent parts.
4. [First effects](https://strudel.cc/workshop/first-effects/) — shaping and processing sounds.
5. [Pattern effects](https://strudel.cc/workshop/pattern-effects/) — time and pattern transformations.
6. [Workshop recap](https://strudel.cc/workshop/recap/) — a compact reminder of the workshop vocabulary.

## References to keep nearby

| Resource | Use it for |
| --- | --- |
| [Mini-notation](https://strudel.cc/learn/mini-notation/) | Sequence, rests, subdivision, repetition, alternation and Euclidean syntax |
| [Understanding cycles](https://strudel.cc/understand/cycles/) | Timing and repeating structure |
| [Pattern alignment](https://strudel.cc/technical-manual/alignment/) | Combining patterns with different time divisions |
| [Function introduction](https://strudel.cc/functions/intro/) | Pattern construction and transformation concepts |
| [Tonal functions](https://strudel.cc/learn/tonal/) | Pitch and scale operations |
| [Synths](https://strudel.cc/learn/synths/) | Waveforms, synthesis and wavetables |
| [Samples](https://strudel.cc/learn/samples/) | Sample selection and loading |
| [Audio effects](https://strudel.cc/learn/effects/) | Envelopes, filtering and processing |
| [Visual feedback](https://strudel.cc/learn/visual-feedback/) | Browser visualizations and their options |
| [Showcase](https://strudel.cc/intro/showcase/) | Performances and musical inspiration |

The [Strudel browser editor](https://strudel.cc/) is useful for following those pages directly. The documentation footer links to Strudel's own community channels; use [Tompouce Discussions](https://github.com/ekollof/tompouce/discussions) for terminal-specific questions and sharing patterns.

## Bring what you learn into Tompouce

Tompouce pins Strudel 1.2.6 and uses a native audio backend. Current upstream docs can describe newer behavior, browser-only devices, visuals or effects. Use the project [native audio reference](https://github.com/ekollof/tompouce/blob/master/docs/javascript.md) to check support; runtime warnings identify unavailable controls. The web's additional GM soundfont variants (beyond the default per name) and advanced wavetable morph/warp processing differ from the browser.

Begin with a built-in synth, then change one thing at a time. Try replacing spaces with brackets, moving a melody through a scale, layering a bass, patterning a filter and adding one view. Compare predictions with what you hear. Keep each experiment in a separate song buffer and save a working version before extending it.

Useful local resources are the [tutorial source](https://github.com/ekollof/tompouce/blob/master/strudeltutor.json), bundled project examples, and [example gallery](https://github.com/ekollof/tompouce/blob/master/docs/examples.md). The gallery uses JavaScript Strudel and is checked directly by the native example test.
