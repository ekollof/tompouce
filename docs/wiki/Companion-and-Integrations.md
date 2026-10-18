# Companion and integrations

## Chromium companion

Press F2 and enter `/companion` to launch the local companion; `/companion stop` stops its helper. It provides source editing, transport, tempo, console, voice activity and the shared sound browser. Audio remains in the native engine. The companion is loopback-only and exits with the engine.

## MCP

Run Tompouce as an MCP server over standard input/output:

```sh
tompouce --mcp
```

Configure your MCP client with the actual executable path and `--mcp` arguments. Main tools cover source retrieval/editing, evaluation, play/stop, tempo, song/track/session management and diagnostics. `music_sounds` searches the shared catalog; `music_reload_sounds` reloads fixed config storage.

`music_set_code` replaces and evaluates source; `music_evaluate` reevaluates the current source. `music_play` starts transport separately. Read-only diagnostics include `music_health`, `music_debug`, `music_get_status` and `music_get_console`. For a shared terminal session, `--mcp --ui` enables UI orchestration. A loopback TCP transport is also available through `--mcp-port`.

See the [MCP setup](https://github.com/ekollof/tompouce/blob/master/README.md#mcp-external-agents) and [architecture](https://github.com/ekollof/tompouce/blob/master/docs/architecture.md). Optional AI requests use an OpenAI-compatible endpoint; consult the README's AI data-egress description before enabling that workflow.

## MIDI

The native backend supports MIDI notes and `midichan` through ALSA output. Check F4 or MCP health for MIDI readiness and the project reference for current startup/device configuration. This is separate from `gm_` synths, which render a local soundfont to audio.

## OSC

Route a pattern to an external receiver such as SuperDirt:

```js
note("c4 e4 g4").s('superpiano').osc()
s("bd sd").oschost('127.0.0.1').oscport(57120).osc()
```

The default destination is `127.0.0.1:57120`. `.osc()` is exclusive external output for that branch; use another branch for simultaneous local audio. The engine sends timestamped UDP `/dirt/play` bundles without a browser bridge. A monotonic clock permits external scheduling even when no local audio backend starts.

Hosts must be `localhost` or literal IPv4/IPv6 addresses; arbitrary DNS names are unavailable. Invalid destinations, complex values and oversized packets are rejected. See the [OSC reference](https://github.com/ekollof/tompouce/blob/master/docs/javascript.md#osc-output) for bounds and conversion semantics.
