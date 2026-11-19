# External plugin hosting

Tompouce does not host plugins inside the engine. Arbitrary plugin code runs in
separate processes, outside the audio callback and outside the QuickJS contexts.
The first usable path needs no Tompouce code at all.

## Zero-code path: Carla as a separate patchbay

[Carla](https://github.com/falkTX/Carla) is a standalone host for LADSPA, DSSI,
LV2, VST2, VST3, SF2 and SFZ plugins, including Wine bridges such as yabridge.
It is GPL-2.0-or-later, so using it as a separate process does not change
Tompouce's licensing. Plugins, their installers and their licenses stay with the
user.

On a PipeWire desktop, keep Tompouce on its default PipeWire output (F4 shows
`backend=PipeWire`) and bridge Carla in through PipeWire's JACK compatibility
layer. No Tompouce setting or pattern change is needed:

1. Load the plugins and build the graph in Carla (GUI: `pw-jack carla`; headless
   needs a saved project: `pw-jack carla -n /path/to/project.carxp`). On a native
   JACK box, run Carla directly in the JACK graph instead of using `pw-jack`.
2. In the Carla patchbay, connect Tompouce's outputs to the plugin chain's audio
   inputs, and the chain's outputs to the hardware sink. With PipeWire you can
   also patch from the shell with `pw-link`: list nodes with `pw-link -l` or
   `pw-dump`, then connect the `tompouce` playback ports to Carla's JACK input
   ports and Carla's outputs to the destination sink.
3. Start Tompouce normally. It renders as usual; Carla processes the signal
   downstream. MIDI CC from `.midi()`/`.midicmd()` patterns can drive Carla over
   JACK MIDI or the PipeWire MIDI bridge if the user routes those ports too.
   There are no pattern parameters for the plugin graph on this path.

This path does not touch Tompouce's source, playback or desktop routing. For
tests, use an owned silent sink and owned browser/profile state, verify the
route before applying nonzero gain, and restore the previous routing afterwards.

### What is and is not verified here

The development box has Carla 2.5.10, `pw-jack` and `pw-link`, and PipeWire is
running. Carla's headless mode requires a project file, and Tompouce's default
output is a PipeWire stream named `tompouce`. The existing
`test-audio-backends`, `profile-prep-backend` and `TOMPOUCE_OUTPUT_TARGET`
checks only cover Tompouce to an owned sink; they do not measure a Carla chain,
Wine bridges or device latency. End-to-end latency therefore remains a
measurement to make on a configured box: capture round-trip MIDI-to-audio
latency under load (for example with `jack_iodelay` or the PipeWire latency
tools), keep the graph quantum low and stable, enable realtime scheduling
(rtkit for native plugins, rtkit/wine-rt for Wine bridges) and avoid an
intermediate PulseAudio hop. Record those numbers before claiming
live-coding suitability; do not infer them from the native engine benchmarks.

## Planned, not implemented

- An `out` control for named PipeWire targets so `s('saw').out('carla')` can
  route individual voices to a dedicated stream (exact node-name match, small
  cap, default-stream fallback with a warning). Sources/capture stay out of
  scope until an input path exists.
- A managed Carla instance driven from patterns, with project files for plugin
  graphs and note/CC through the existing MIDI output.
- Native LV2 discovery/instantiation through `lilv`, with audio and note/CC
  plumbing in a confined worker process on the `csound_host` pattern. Plugins
  must never run in the engine process, and preparation stays outside the audio
  callback. VST3 is optional and strictly after LV2; VST2 is out.

See [TODO.md](../TODO.md) for the current state of those items and
[audio backends](../README.md#run) for Tompouce's output selection rules.
