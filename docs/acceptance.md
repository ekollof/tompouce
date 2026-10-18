# Tompouce manual acceptance checklist

Build: `cmake --build build --clean-first` must finish with zero warnings under
`-Wall -Wextra`. Mark each box after verifying on a real terminal
(kitty or xterm-256color).

## Build and launch

- [ ] `cmake --build build --clean-first` succeeds, no warnings/errors.
- [ ] `./build/tompouce` opens the Notcurses UI (status bar, code pane, console
      pane, prompt line) with no crash.
- [ ] `./build/tompouce --mcp` stays headless (no UI) and answers
      `{"jsonrpc":"2.0","id":1,"method":"ping"}` with `{"result":{}}`.
- [ ] `./build/tompouce --mcp --ui` from a terminal shows the UI and accepts keys.

## Editor and vi mode
- [ ] Typing inserts text at a visible cursor (block highlight + hardware cursor).
- [ ] Arrows / Home / End / Backspace / Delete / Enter behave.
- [ ] Status bar shows `[INSERT]` by default.
- [ ] `F3` toggles to `[VI:NORMAL]`; `Esc` returns to `[VI:NORMAL]` from insert.
- [ ] In vi normal: `h j k l`, `0 ^ $`, `gg`, `G` move; `i a I A o O` enter
      `[VI:INSERT]`; `x` deletes a char; `dd` deletes + yanks a line;
      `yy` yanks; `p`/`P` paste below/above.
- [ ] Pasting works after `dd` without a prior `yy`.

## Help and state panes

- [ ] `F1` swaps the console pane for the cheatsheet; status bar keeps
      `F1:help`; `F1` again restores the console.
- [ ] Cheatsheet shows track syntax, pattern operators, transforms, sounds,
      limits, and keys; headers/code stand out (reverse video).
- [ ] `PgUp`/`PgDn` scroll the cheatsheet (scroll hint `n/m` top-right).
- [ ] `F4` shows the state pane (bpm, transport, queue depth, voices,
      tracks with `fast`/`slow`/`rev`/`gain`/`inst`/cycle); `F4` hides it.
- [ ] Code is color-highlighted (keywords, notes, drums, transforms,
      comments, rests, numbers); degrades sanely on mono terminals.

## Completion and bank search
- [ ] `Tab` on an empty/first word lists keywords; `Tab` after `drum `
      completes drum tokens; after `inst ` offers `wt:<tables>`.
- [ ] `F6` opens bank search: matching banks/kits only (kit pieces with
      a matching parent are collapsed; empty query lists top-level banks).
- [ ] `Enter` on a bank appends `| bank <dir>` to a drum line (or adds a
      `drum ... | bank <dir>` line); `Esc`/`F6` closes.
- [ ] While indexing, pane shows `indexing... N files`; `TOMPOUCE_BANKS`
      override is honored when set.
- [ ] Second launch loads the index from LevelDB in ~ms (console shows
      `banks index loaded`); touching a bank dir triggers a rescan.

## Sampler

- [ ] `drum bd sd hh cp | bank rolandtr909` plays 909 samples; console
      shows the resolved bank dir.
- [ ] `| bank Dirt-Samples/bd` plays (slot mapping for cryptic names).
- [ ] `| bank nope` falls back to synthesis with a console note.
- [ ] `lead c4 eb4 | bank Horn/mute` plays horn samples pitched to notes
      (nearest recorded pitch wins).
- [ ] Sampled melodic notes stop at note end + `rel` (default 0.08s);
      drum/`s` banks ring fully.
- [ ] Wavetable track (`lead c4 e4 | inst wt:pad`) sounds and shows the
      table name in the state pane.
- [ ] State pane collapses automatically on very small terminals.

## Evaluate and audio

- [ ] Paste `docs/examples.md` §11 (full mix), press `F5`: console shows
      `Loaded 5 tracks`, status shows `trk:5`.
- [ ] Audio plays (kick/snare/hats/bass/lead audible); `Ctrl+P` toggles
      `PLAY`/`STOP`.
- [ ] `F2`, type `/bpm 140`, Enter: status `BPM:140`.
- [ ] `F2`, type `/play` and `/stop`: transport follows.
- [ ] `F2`, type `/clear`, Enter: editor empties, console logs `Code cleared`.
- [ ] `F2`, type a nonsense `/bogus` command: console logs `Unknown command`,
      no crash.
- [ ] Alternation advances per cycle: `<bd sd>` flips kick/snare each cycle.
- [ ] `slow 2` part is silent on odd cycles; `rev` mirrors timing; `fast 2`
      doubles density.
- [ ] `lead [e2 ~]*8`, `euclid(3,8,bd)`, `every(2,sd)` expand correctly.
- [ ] Typos (`[bd sd`, `euclid(9,8,x)`) keep the valid prefix playing and
      log `pattern: ... at col N` in the console.
- [ ] Live checking: typing `drum [bd sd` shows `! L1: expected ]` in the
      status bar with a red gutter marker; closing the bracket clears it.
- [ ] `ctest --preset default` passes (pattern gate cases).

## Mix, fx, MIDI, sessions

- [ ] `euclid(3,8,bd)` and `every(2,sd)` play the expected rhythms.
- [ ] `| shuffle` varies order per cycle; `| degrade 0.5` thins events.
- [ ] `| mute` silences a line; `| solo` solos all solo lines.
- [ ] `| midi 0` emits notes on ALSA client `tompouce` (check `aconnect`).
- [ ] `fx drive/cutoff/delay/verb` lines take effect without clicks/dropouts.
- [ ] `s <bank-path> ...` plays one-shots; missing files are silent.
- [ ] `music_session save/load/list/delete` round-trips; `../x` rejected.
- [ ] `music_health` reports `stream=up`, voices, queue, banks, midi.
- [ ] `music_track move/remove` reorder and delete; `strudel://tracks`
      lists `{index, active, preview}`.
- [ ] `--bpm 140 --code 'drum bd'` starts primed; `--mcp-port` answers ping.
- [ ] Quit persists `~/.config/tompouce/state.json`; relaunch restores.
- [ ] `F10`/`F11` move the split; small terminals don't flicker.
- [ ] No `ALSA lib ...` text ever appears inside the UI (silenced via
      `snd_lib_error_set_handler`; verify by snapshotting rows 2-20).
- [ ] `F12` shows a piano roll (pitch bars per voice, C-row labels) plus a
      live braille scope of the output; ASCII fallback without UTF-8 locale.

## MCP (headless)

- [ ] `initialize` returns `protocolVersion`, `serverInfo.name == tompouce`.
- [ ] `tools/list` contains all eleven `music_*` tools.
- [ ] `music_set_code` with §11 returns `Loaded 5 tracks.`
- [ ] `music_get_status` reflects bpm/track count/playing.
- [ ] `music_get_code` returns the exact source just set.
- [ ] `music_get_console` returns recent `Loaded …` lines.
- [ ] `music_play` / `music_stop` toggle playback.
- [ ] `music_set_bpm {"bpm":140}` → `BPM set to 140.`; `{"bpm":1e999}` or
      a string is rejected as invalid (no NaN/inf leak into scheduling).
- [ ] `resources/list` shows `strudel://code`; `resources/read` returns it.
- [ ] Unknown method returns JSON-RPC `-32601`; bad `tools/call` args return
      `-32602` or `isError` text, server keeps running.

## MCP + UI sync

- [ ] Launch `--mcp --ui` from a pipe-fed harness, send `music_set_code`:
      editor content updates on screen and evaluates.
- [ ] Interleaved typing + incoming MCP code does not corrupt the editor.

## AI

- [ ] With no `OPENAI_API_KEY`: `F2` prompt or `music_ai` returns an offline
      template within seconds and loads it.
- [ ] With a key set (or bogus key + unreachable base URL): falls back to
      offline template, no hang, no crash.
- [ ] Prompt input caps at 240 chars (no status-bar overflow).

## Resize and redraw

- [ ] Shrink the window: layout reflows, no crash, both panes visible.
- [ ] Grow the window back: UI fills the new size within a frame or two.
- [ ] Rapid resizes do not corrupt the editor buffer or cursor position.

## Security regression

- [ ] Send a >1 MiB single line to `--mcp` stdin, then `ping`: oversized
      frame is dropped, server answers `ping`.
- [ ] `drum bd*100000000` loads fine (repeat clamped, no hang/OOM).
- [ ] 40-deep `[[[[…]]]]` nesting loads fine (depth-capped, no crash).
- [ ] No `strcpy`/`strcat`/`sprintf`/`gets`/`system` remain in `src/` + `include/`
      (`grep -rnE '\b(strcpy|strcat|sprintf|gets|system|popen)\s*\(' src include`
      returns nothing).

## Song mode, tracks, save/load

- [ ] `Ctrl+T` copies the whole buffer to a new track (next in list, becomes
      active); empty buffer logs `nothing to copy`.
- [ ] `F9` shows the track pane with one row per track (`> i: preview`);
      active row highlighted; `F9` hides.
- [ ] `Ctrl+N` / `Ctrl+B` step through tracks (wraps), loading each into
      the editor; `F5` commits editor text back into the active track.
- [ ] `F7` starts song mode at track 0; each finished cycle advances;
      status shows `SONG n/m`; typing is never clobbered by auto-advance.
- [ ] `F8` holds the current track (status `HOLD`); `/song hold` too.
- [ ] `music_track list` shows `*active` marker; `goto`/`copy` follow editor.
- [ ] `Ctrl+D` duplicates the current line below and moves the cursor there.
- [ ] `Ctrl+S` opens a file picker and writes valid JSON (`app`, `version`,
      `bpm`, `code`, `tracks`, `active`); editing then `Ctrl+O` restores the
      whole song (every track) and bpm.
- [ ] `/save /tmp/x.json` + `/load /tmp/x.json` round-trip; `/load` of
      garbage logs `bad json` without crashing.
