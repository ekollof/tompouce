# TODO.md

## Roadmap (do not lose this)

Live goal: Strudel-shaped code in the terminal, with the widgets drawn
*beside* the text. The source buffer stays plain text.

### Why not Bison
Do **not** replace the pattern parser with Bison (or any all-or-nothing
LALR compiler-compiler). `AGENTS.md` already records the reason: a live
coder's typo must yield a partial pattern plus `pattern_last_error()`,
and an LALR parser throws the whole line away. Keep the hand-written
lexer and recursive descent. Grow it. A second grammar for statements
(`note("…").lpf(800)`) can sit next to it, still hand-written, because
the constructs are a small fixed set of calls and dotted methods.

### Now
- [ ] Song editor follows the playing step when the buffer is clean.
      Auto-advance never set the follow flag, so the editor stuck on
      one track while the audio moved on. Dirty edits must not be
      overwritten (F5 commits).
- [ ] Song list grows on the heap (exact-size strings). 32 × 64 KiB
      slots go away. Safety ceiling only, so a client cannot exhaust
      memory. One buffer still caps at the 64 KiB editor.
- [ ] Mini-notation closer to https://strudel.cc/learn/mini-notation
      and the workshop recap: `!` replicate, `@` elongate, `/` slow
      across cycles, `?` degrade (seeded by cycle so lookahead matches),
      `bd(3,8)` / `bd(3,8,rot)`, `{a b, c}` polymeter, `|` choice
      *inside* a pattern. `|` before a transform stays the track bar.
- [ ] Statement form from the workshop, one pattern per line:
      `setcpm`, `note` / `n` / `s` / `sound`, `.gain` `.lpf` `.pan`
      `.fast` `.slow` `.rev` `.attack` `.decay` `.sustain` `.release`
      `.scale` `.bank` `.room` `.delay` `.degrade` `.degradeBy`,
      and `slider(v)` / `slider(v,min,max)` / `slider(v,min,max,step)`.
      Patterned numbers (`gain("[.25 1]*4")`) query by time.
- [ ] Widgets are not inline HTML. Draw a slider bar on the code row,
      list sliders in the state pane, nudge with Ctrl+Left / Ctrl+Right.
      A one-row piano roll of the cursor line and a one-row scope sit
      on the bottom of the code pane. F12 keeps the full roll.
- [ ] Tests taken from strudel.cc workshop pages (simple through a
      multi-method line), in `tests/test_strudel.c`. `make test` stays
      ALL PASS.

### Later (real Strudel still does these; we do not yet)
- Signals: `sine`, `saw`, `perlin`, `.range()`, `.slow()` on a signal.
- `.vowel`, `.jux`, `.ply`, `.off`, `.add` on a pattern, `.hurry`,
  `.late` / `.early`, `stack()` of *different* functions on one line.
- Euclidean arguments that are themselves patterns (`<3 5>(5,8)`).
- Mondo notation (`#`), FM / distortion / postgain, sample speed `-1`.
- General MIDI and Dirt sample *names* beyond banks we already index.
- Quoted strings that span editor lines without a closing quote on
  line 1 are joined while loading; the editor itself is still one
  buffer, not a Strudel REPL cell.
- `setcpm(x)` maps to BPM `x * 4` because one cycle here is 4 beats.
  BPM is still clamped 20–400.

### Shipped earlier

## Near-term polish
- [x] Split pane resizing / adjustable console width. (F10/F11 split)
- [x] Fix top-of-screen flicker on small terminals. (no per-frame cls; full-width padded writes)
- [x] Add syntax highlighting in the editor. (colors + live error gutter)
- [x] Persist last BPM/code to `~/.config/strudelterm/`. (state.json)
- [x] Add `--bpm`, `--code` CLI flags. (plus `--block`, `--mcp-port`, `--help`)

## Parser / music features
- [x] Add arbitrary sample loading via `s`/`file` directives. (`s <bank-path>` tracks + `| bank`)
- [x] Euclidean rhythms (`euclid(k,n)`), rotors, `every(n, ...)`. (`euclid(k,n,tok[,rot])`, `every(n,X)`; rot = rotation arg)
- [x] Effects chain: reverb, delay, lowpass, distortion. (`fx drive/cutoff/delay/verb` master chain)
- [x] `stack`, `cycle`, `shuffle`, `chunk`, `degrade`. (`| shuffle`, `| degrade`; multi-line already stacks; cycle/chunk deferred)
- [x] MIDI output (OSC/MIDI) for external instruments. (ALSA `| midi CH`; OSC deferred)

## Audio engine
- [x] MP3/FLAC decode in sampler (vendored minimp3/dr_flac): unlocks
      `dough-samples/piano` (mp3) and `salamander-grand` (flac).
- [x] Automatic voice stealing by priority/recent-play. (quietest-first)
- [x] True amplitude envelope release starts on duration/gate, cleanup. (`| att`, `| rel`, sample gating)
- [x] Hard limiter/soft-clip tuning, DC filter. (tanh + DC blocker)
- [x] Configurable audio buffer size/block. (`--block`)

## MCP / agent
- [x] Structured track listing resources (per-track summary). (`strudel://tracks`)
- [x] Track primitives (mute/solo/move, set color/preset). (mute/solo transforms, move/remove; color/preset deferred — no color system)
- [x] Save/load project files; agent tools for composition sessions. (Ctrl+S/O, /save//load, `music_session`)
- [x] Optional TCP socket transport for long-running agent composition. (`--mcp-port`, loopback only)
- [x] Add diagnostics/health endpoint for audio readiness. (`music_health`)

## Documentation
- [x] Add example code gallery. → `docs/examples.md`
- [x] Add architecture diagram. → `docs/architecture.md`
- [x] Add manual acceptance test checklist. → `docs/acceptance.md`
