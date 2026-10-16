# StrudelTerm

A terminal live-coding environment for music, inspired by Strudel, built in C.

## Components

- `src/audio.c` — PortAudio real-time synth + drum voices
- `src/pattern.c` — mini-notation parser (sequences, groups, comma stacks, `*`, rests, `<>`)
- `src/harmony.c` — Strudel chord symbols, scales, and scale degrees
- `src/scheduler.c` — cycle-based scheduler with event queue
- `src/track.c` — code-to-tracks compiler
- `src/ai.c` — LLM-assisted code generation (OpenAI-compatible)
- `src/tui.c` — S-Lang terminal interface (editor, console, AI prompt)
- `src/wavetable.c` — procedural bandlimited single-cycle tables (`inst wt:<name>`)
- `src/mcp.c` — MCP JSON-RPC server + UI orchestration

## Docs

- [`docs/examples.md`](docs/examples.md) — copy-pasteable example gallery
- [`docs/architecture.md`](docs/architecture.md) — module map, data flow, threading, limits
- [`docs/acceptance.md`](docs/acceptance.md) — manual acceptance test checklist

## Build

```sh
make
```

Requirements: `slang` (S-Lang), `portaudio-2.0`, `libcurl`, `libcjson`, `leveldb`.

## Run

```sh
./strudelterm
```

Keys:
- Type code in the editor (left pane).
- `F5` or `Ctrl+R` — evaluate code
- `Ctrl+P` — play / stop
- `F1` — toggle language cheatsheet over the console pane (`PgUp`/`PgDn` scroll)
- `F2` — AI prompt (supports `/bpm N`, `/play`, `/stop`, `/clear`, `/save`, `/load`, `/song`, `/rescan`)
- `F4` — toggle internals/state pane (bpm, voices, queue, tracks)
- `F3` — toggle vi-ish editor mode (normal/insert: `hjkl`, `gg/G`, `dd`, `yy`, `p/P`, `x`, `i/a/o/O/I/A`, `0/$`)
- `Tab` — context-aware completion (keywords, notes, drums, transforms, `wt:` tables)
- `F6` — browse sample banks (`vsco2-ce/Brass`…); `Enter` descends into a folder or uses it as `| bank <dir>`, `Tab` always uses it, `Backspace` goes up
- `F7` — song mode: play track buffers in sequence from track 0 (`/song [on|off|hold]`)
- `F8` — hold current track (loop it inside song mode)
- `F9` — track list pane (shows buffers, active highlighted)
- `F10` / `F11` — narrow / widen the code pane
- `F12` — piano roll + braille scope pane (per-voice pitch bars, live output scope)
- `Ctrl+T` — copy entire buffer to a new track (next in list)
- `Ctrl+N` / `Ctrl+B` — next / previous track (loads into editor)
- `Ctrl+D` — duplicate current line (vi `yy`+`p` does this too)
- `Ctrl+S` / `Ctrl+O` — save / load the whole song (all tracks) via a file picker (`/save [path]`, `/load <path>`)
- `Ctrl+Q` — quit (session auto-saved to `~/.config/strudelterm/state.json`)
- Arrows / Enter / Backspace — edit

Flags: `--bpm N`, `--block N` (audio frames 64–4096), `--code '...'`,
`--mcp-port N` (TCP instead of stdio), `--mcp [--ui]`, `--help`.

Language extras follow [Strudel](https://strudel.cc) names: comma chords (`c4,e4,g4`),
`n` / `chord` with ireal symbols (`Cm7`, `C^7`), `| scale C:minor` (degrees, root octave 3),
`| arp 0 1 2`, `| sound sawtooth`, `| adsr .01:.1:.6:.2`, `| lfo freq 4 1`, `| vib 4:.1`,
`| transpose N`, `| lpf Hz`. Also `s <bank-path>`, `fx drive|cutoff|delay|verb X`,
`euclid`, `every`, `| shuffle`, `| degrade`, `| mute`, `| solo`, `| midi CH`.
Pink and brown are aliases of white noise. `| lpf` is per voice; `fx cutoff` stays the master filter.
Session auto-restores on launch; `/rescan` picks up added banks.

Example code:

```
bpm 128
drum bd sd [bd bd] sd
drum hh hh hh hh | fast 2
bass c2 e2 g2 c3 | rev
lead c4 e4 g4 c5
```

AI uses `OPENAI_API_KEY` (and optional `OPENAI_BASE_URL`, `OPENAI_MODEL`). Without a key, offline templates are used.

### AI data egress
When `OPENAI_API_KEY` is set and the network library initializes, the AI prompt **and the current code buffer** are sent to the configured `OPENAI_BASE_URL` endpoint. Do not put secrets in the code buffer. Point `OPENAI_BASE_URL` at a local model if you need to keep data on-host. If curl initialization fails, AI silently falls back to offline templates.

## MCP (external agents)

Run as an MCP server over stdio:

```sh
./strudelterm --mcp
```

Tools: `music_set_code`, `music_evaluate`, `music_play`, `music_stop`, `music_set_bpm`, `music_get_status`, `music_get_code`, `music_get_console`, `music_ai`, `music_song`, `music_track`, `music_session`, `music_health`. Example Claude Code config:

```json
{
  "mcpServers": {
    "strudelterm": {
      "command": "/home/ekollof/Work/music/code/strudelterm",
      "args": ["--mcp"]
    }
  }
}
```

## Terminal UI

The interactive UI now uses S-Lang (`slang`) instead of ncurses. Works in any terminal, including kitty.

## Sample banks (`assets/banks`)

Now live: `| bank NAME` (drums + pitched), `s <bank-path>` one-shots, F6
search. Licenses verified against the downloaded trees — this section is
the required attribution for the CC-BY / CC-BY-SA banks.

| Directory | Content | License | Author / upstream |
|---|---|---|---|
| `akwf-free` | ~4000 single-cycle waveforms (WAV + C/JS) | CC0-1.0 (`LICENSE.md` in tree) | Adventure Kid, via <https://github.com/KristofferKarlAxelEkstrand/AKWF-FREE> |
| `dough-samples` | Strudel default manifests (mostly remote refs) + local `piano/` subset | mixed, per bank below | Felix Roos, <https://github.com/felixroos/dough-samples> |
| `dough-samples/piano` | Salamander piano subset | CC-BY 3.0 | Alexander Holm |
| `mrid` (+ `dough-samples/mrid` ref) | Mridangam samples | CC-BY-SA 4.0 — share adaptations alike | © Arthur Carabott 2022, performer Harishankar V Menon |
| `VCSL` | Community sample library | CC0 | Sam Gossner / Versilian Studios, <https://github.com/sgossner/VCSL> |
| `Dirt-Samples` | SuperDirt/TidalCycles sample set | per upstream repo | TidalCycles, <https://github.com/tidalcycles/Dirt-Samples> |
| `tidal-drum-machines` | Drum machine collection | per upstream repo | <https://github.com/ritchse/tidal-drum-machines> |
| `vsco2-ce` | Chamber orchestra (SFZ) | CC0 (`LICENSE` in tree) | Recorded by Sam Gossner & Simon Dalzell, <https://github.com/sgossner/VSCO-2-CE> |
| `salamander-grand` | Yamaha C5 grand (SFZ) | CC-BY 3.0 (`LICENSE` in tree) | Alexander Holm |
| `fluidr3` | General MIDI SoundFont (SF2 + SF3) | MIT (`original-files/COPYING` in tree) | © 2000-2002, 2008 Frank Wen |

Notes:
- CC0 / MIT / public-domain banks need no permission and are credited above anyway.
- CC-BY 3.0 banks (Salamander piano): keep this attribution when redistributing.
- CC-BY-SA 4.0 (`mrid`): adaptations must be shared under the same license.

### Adding banks

Drop directories of `.wav` / `.mp3` / `.flac` anywhere under `assets/banks/`
(or point `STRUDELTERM_BANKS` elsewhere). Layout tips: flat per-instrument
dirs for drums (`mydrums/bd/*.wav`), note-named files (`_C4_`) for pitched
use, keep playable files within 8 levels. Then `/rescan` (no restart
needed). Only libre-licensed material — keep the table above current.
The index caches in LevelDB (`~/.cache/strudelterm/banks.ldb`).