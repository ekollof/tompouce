# TODO.md

## Terminal graphics (2026-10-18)

- [x] Add native punchcard, spiral, pitchwheel, spectrum, wordfall and tscope alias, with Kitty graphics and braille/ASCII fallback.
- [x] Query final tagged haps for all note views, including downstream time changes and held/upcoming/silent patterns, so piano rolls follow playback.
- [x] Support patterned active-token colors and basic markcss terminal attributes; document browser-only option limits.
- [x] Verify all seven native views at pixel/braille resolution, spectrum frequency detection, downstream transformations, and 49 terminal PTY cases.
- [x] Automatically enhance inline scopes and piano rolls with Kitty graphics.
- [x] Target 60 FPS with rendering-aware UI waits; transmitted-frame test measured about 59 FPS.
- [x] Preserve distinct overlapping/repeated piano notes with onset markers, per-pitch lanes and taller graphics rolls; verify all 90 Arpoon piano events.
- [x] Add note/octave and keyboard y-axis guides to graphics rolls; keep waveform scopes with correctly scaled amplitude labels per user preference.
- [x] Retain braille fallback and persistent `/graphics auto|braille` selection.
- [x] Clip/clean images during scrolling, resize, hiding widgets and exit.

## Project rename (2026-10-18)

- [x] Rename executable, UI/companion branding, installer, packages and docs to Tompouce.
- [x] Preserve existing settings, sessions, imports, caches and bank environment compatibility.
- [x] Verify native suite, 34 PTY cases, Python/browser tests and a temporary-prefix install.

## Cross-machine continuation (2026-10-18)

Read `AGENTS.md` "Cross-machine handoff" for transfer paths, dependencies,
build commands, measured evidence, and the browser-comparison recipe.
This checkpoint is documentation only; no live source/playback was changed.

Destination verification (2026-10-18): the working tree, bundled runtime,
compositions, and installed piano/drum-machine catalogs are present here.
No Tompouce process or MCP listener was running at inspection. Installed
the checksum-verified Csound 7.0.0-beta.18 package using the bundled installer,
then rebuilt cleanly with Csound enabled. `make -j4 test` passed, including
31 published examples / 1,207 reference events, native Csound rendering and
execution-budget checks, and actual callback tests. Installed-bank checks
ran for C4 (713,786 stereo frames) and TR909 snares (12,766 stereo / 7,858
mono frames). All 11 PTY checks and `git diff --check` passed. The next step
remains the user's listening comparison with Arpoon in a fresh process;
no composition or sound algorithm was changed during this verification.
The user subsequently launched `./tompouce --mcp --ui` and loaded Arpoon.
Read-only MCP at 127.0.0.1:18789 confirms playback at BPM 120, the saved
Arpoon source, and gapless stereo piano lengths C4 713,786 / Fs3 711,201 /
Ds4 697,571. No sample-fetch errors appeared in the console snapshot.
The fresh process is using the decoder fix. The user reports that Arpoon's
opening piano phrase "Sounds similar now" against strudel.cc. This closes
the pending fresh-process listening check, without claiming full audio parity.

- [x] Verify the complete working tree, including untracked implementation,
      bundled JS, vendored sources/licenses, fixtures, and compositions.
      A clone of current HEAD or a tracked-only diff omits major changes.
- [x] Verify the ignored sample banks with manifests and relative
      layout intact. Set `TOMPOUCE_BANKS` if the new root differs.
      Preserve piano and tidal-drum-machines catalogs, not just audio files.
      Restore wanted editor config, UI state, and named sessions separately;
      rebuild the bank index for the new paths.
- [x] Install native dependencies (including tree-sitter and xxd), and
      Csound 7 for current upstream orchestras. The bundled installer is
      Linux x86_64 only; rebuild for the destination architecture/prefix.
- [x] Run `make clean`, `make -j4`, `make test`, and
      `python3 tests/test_tui_pty.py` on the destination. Last local results:
      full suite passed, 31 examples / 1,207 event references, all 11 PTY
      cases passed. Network-dependent and installed-bank checks need their
      corresponding resources; synthetic decoder/catalog fixtures are offline.
- [x] Load saved Arpoon in a fresh process and verify the gapless MP3 fix
      by listening against strudel.cc. The old live process had not restarted
      for that fix. No octave shift is justified by the measured event data.
      Check C4 = 713,786 stereo frames and TR909 snare slots 0/2 =
      12,766 stereo / 7,858 mono frames; sample IDs are process-local.
- [x] Capture matching native/browser
      passages and isolate remaining sample/envelope/effect differences.
      Do not compensate with song-specific patches or note transposition.
      Dry piano and custom IR comparisons now pass; generated reverb remains
      a measured algorithm difference, rather than a note/sample mismatch.
- [x] Turn the manual browser comparison into a maintained audio capture
      harness with explicit versions, inputs, and measured tolerances.
      The 720-event comparison and .976 opening correlation were manual
      diagnostics. `/tmp` WAVs/helpers are disposable and not regression
      fixtures; the Node/V8 event corpus is not an independent audio oracle.
      Implemented `make compare-audio`; see `docs/audio-comparison.md` for
      inputs, provenance, tolerances, and the diagnostic-only wet comparison.

The user subsequently authorized gap closure, prioritizing time-stretch and
convolution, and selected a Chromium companion tab for visuals and controls.
Implemented: browser capture harness, installed VCSL/mridangam default maps,
confined local imports, native sample pitch compensation/custom convolution,
and the loopback companion. See `docs/javascript.md` for limits and
`docs/audio-comparison.md` for measured comparisons. ZzFX, bytebeat and
Kabelsalat remain later work; generic browser APIs remain out of scope.
The old machine's live MCP
process must stay untouched; reconnect to the actual destination endpoint
instead of assuming the old session or temporary browser exists there.

Gap-closure verification: `make -j4 test` passed with Csound enabled and the
31 examples / 1,207 event corpus unchanged. All 11 PTY checks and four
companion bridge tests passed, as did JS syntax and whitespace checks.
Browser gates passed for dry C4 (correlation 0.99999994), Arpoon without
room (0.999986), and a custom stereo convolution IR (0.99999997, RMS ratio
0.9931). Wet generated reverb is diagnostic only. The companion bridge is
running at http://127.0.0.1:18990/ in this session; it connected read-only to
the user's MCP session. Its UI still needs user visual inspection because
no browser UI surface is available to this agent. New native features need
a user restart; the live process was not restarted or edited.

## Gap closure (2026-10-17)

Checked against the tree, not against an older status list. Update this
section as items move. This section records the previous machine's work;
use "Cross-machine continuation" above as the immediate checklist.

Already in this tree (do not reimplement):

- [x] Stereo WAV/MP3/FLAC stays interleaved. Playback uses both channels
      in `src/sampler.c` and `src/audio.c`. More than two channels keeps
      the first pair (WAV averages the rest onto both sides).
- [x] Pitch-keyed sample maps accept arrays per region. Indices use
      JavaScript `Math.round`, then wrap inside the region, including
      negatives. `tests/test_websamp.c`.
- [x] `.csound(1.25)` and `.csoundm(...)` schedule numeric instruments
      and MIDI pfields. `tests/test_javascript.c`.

Shipped in this pass:

- [x] Editor source grows until malloc fails. Each line is its own heap
      string. Offsets are int, so one buffer stops near 2 GiB. Long lines
      wrap by default (`/wrap off` sidescrolls; `"wrap"` in config.json).
      `tests/test_editor.c`.
- [x] Separate effect orbits and ducking (`orbit` / `duckorbit`).
      Twelve buses. `tests/test_audio.c`.
- [x] Browser-comparison checks for the saw, the ADSR, and the biquad Q.
      These are curve checks, not bit-identical WebAudio output.
      `tests/test_dsp.c`. A browser capture harness is still open.
- [x] Default drum map: installed Dirt-Samples manifest, overlaid with
      the current uzu-drumkit manifest. Verified all 16 uzu names/slots
      against upstream. User `samples()` remains separate and wins.
      Offline tests cover relative bank roots, manifest order, and
      rejected symlinks/traversal. No uzu audio is fetched by the tests.
- [x] Native sample slices follow the upstream envelope gate rather
      than cutting off at `.end()`. Release tails, reverse playback,
      explicit duration gating, loop prefixes/releases, and both stereo
      effect channels are covered in `tests/test_audio.c`.
- [x] Full `make -j4 test` and all 11 terminal PTY checks pass.
      `misc:2` queries produce delay .5 and delaytime 1/3 correctly.

Next verification:

- [x] Register the installed pitch-keyed piano manifest independently of
      `.piano()`. Bare `s('piano')` now selects the proper recording/root
      for each fractional pitch instead of an unrelated AKWF waveform.
      Defaults share pitch-region/array selection with explicit maps.
- [x] Register the installed drum-machine manifest for `.bank(...)`,
      preserving declared array order across all machines. Regression
      tests decode RolandTR909 `sd:0` as `naredrum.wav` (12,766 stereo
      frames) and `sd:2` as `sd02.wav` (7,858 frames), and C4 piano as a
      full stereo recording. Default limits now allow 2,048 names,
      1,024 slots per name, and 16,384 slots overall; the installed
      catalogs require 909 names and include 108-slot banks. Synthetic
      offline fixtures cover these limits, pitch-keyed arrays, signed/
      fractional indices, and user-map precedence. `make -j4 test` passes.
- [x] After the user restarted, read-only MCP confirmed full stereo piano
      recordings and RolandTR909 snare slots at 12,766 stereo / 7,858 mono
      frames. Its song/source/playback were left untouched.
- [x] Investigate the reported piano octave/opening-phrase mismatch against
      actual strudel.cc in a separate headless browser. Across 128 cycles,
      all 720 piano onsets, pitches, and sample playback rates match.
      The C4/Fs5 recordings also match the browser CDN files byte-for-byte.
      Native rendering confirms C4 at its expected pitch; no octave offset
      was applied to the composition, catalog, or controls.
- [x] Fix the confirmed MP3 timing mismatch: skip Xing/Info metadata frames
      and honor encoder delay/end padding before resampling. Piano attacks
      previously included 2,257 unwanted frames (~51 ms at natural speed,
      varying with playback rate). C4 now has 713,786 frames, Fs3 711,201,
      and Ds4 697,571, matching WebAudio. Offline fixtures cover stereo
      MPEG-1, mono MPEG-2/resampling, missing tags, corrupt frame counts,
      and truncated files. The corrected opening piano render aligns
      with the browser at zero offset (correlation .976 over the first
      .6 seconds, including native effects). Full audio parity is not claimed.
- [x] Listen to the corrected opening phrase in a fresh destination process
      (or after the next user restart on the old machine). The old running
      process still had the decoder/cache from before the gapless MP3 fix;
      playback/source were left untouched. Tracked in the portable list above.
- [x] After the user restarted, checked Belldub through read-only MCP.
      `hh`, `mt`, `lt`, `misc`, and `ht` now use decoded samples
      (IDs 0, 1, 2, 3, and 5 in that process). `hh` is 15,439 frames,
      replacing the old Dirt file; `misc` has delay send .5. The toms
      preserve their stereo channels. Bass/chord filters and fractional
      pitch pairs remain intact. No live source or playback changes made.
- [ ] Compare the restarted engine's audio with browser playback.
- [x] Add a browser audio capture/comparison harness; event agreement
      and native DSP regression checks do not establish audio parity.

Deferred feature backlog after fidelity verification (separate scope):

- [x] Sample pitch compensation with independent transport (overlap-add
      grains), and partitioned stereo convolution with registered sample
      impulses. Native callback tests cover doubled duration at preserved
      pitch in both directions, stereo phase, and convolution against a
      direct reference. Custom IR browser capture passes at correlation
      0.99999997. Browser phase-vocoder/generated-reverb parity remains open.
- [x] Installed VCSL/mridangam defaults, with decoded URL path safety checks.
- [x] Chromium companion for native session visuals and controls. Bridge
      checks use a disposable MCP stub; the live connection was read-only.
      Visual QA remains pending because browser UI tools are unavailable.
- [x] Launch companion from the terminal: F2 → `/companion`; stop with
      `/companion stop`. Reuses this engine's TCP port or enables a private
      ephemeral listener for normal UI launches. Owns/reaps the helper,
      captures startup errors, prevents duplicates and stops it on exit.
      Full suite, four bridge tests and 13 PTY cases pass, including launcher
      integration with a browser stub and source readback from that engine.
- [x] Responsive companion voice activity: separate up-to-30-Hz telemetry,
      animation-frame rendering, sample-position interpolation, stale-data
      clearing and reduced hidden-tab polling. Native UI waits wake on MCP
      socket activity. A 20-snapshot PTY burst took 12 ms; frontend scheduling
      tests prove slow metadata cannot block voice updates. Full suite and
      all 13 PTY cases pass.
- [x] Syntax-aware companion source editor: locally bundled CodeMirror with
      JavaScript highlighting, multiline paste, line numbers and undo.
      Preserves selection/history on polling and the editing baseline when
      a source response arrives after a paste. DOM tests exercise actual
      paste and highlighted spans; bridge tests cover the bundle and nonce
      CSP. Rebuild with `make companion-bundle`; restart only the bridge.
- [x] Shared categorized sound browser in F6 and the companion: samples,
      drum-machines, synths, wavetables, user maps and imported sounds.
      Search/counts/snippet insertion; retained local bank browsing.
      Folder import copies WAV/MP3/FLAC into confined persistent config
      storage, preserves alphabetical slots, reloads without changing source
      or playback. Native import is a background helper; browser uses a folder
      chooser. Import, catalog, bridge, editor and DOM tests cover the flows.
- [x] Inline native braille scopes/waveforms and piano rolls beneath
      `.scope()` / `._scope()` and `.pianoroll()` / `._pianoroll()` calls.
      Syntax-aware source placement ignores strings/comments. Display rows
      stay out of source, cursor movement, mouse placement and selection;
      wrapping and horizontal scrolling share the layout. `/inline off|on`
      persists; F12 remains independent. Scope and piano roll now isolate their
      attached pattern via call-site IDs, voice taps and tagged note history.
      No changes to audio routing. Scoped waveform taps precede shared orbit
      effects/master processing; Csound isolation and static file previews
      remain outside this implementation. Up to 32 sites; stale generations
      and unapplied editor changes cannot show another pattern’s samples.
- [ ] ZzFX, bytebeat, and Kabelsalat. Browser synths. Do not evaluate
      their programs inside the audio callback.
- [x] Confined local JavaScript imports under the config modules directory.
      Static/dynamic ES imports, bundled dependencies and sibling imports
      work; traversal, symlinks and files over 256 KiB are refused. Remote
      and npm specifiers stay refused. New file loads cannot run in queries.

## JavaScript runtime update (2026-10-17)

The JavaScript dialect now evaluates using upstream Strudel core/mini/tonal/
transpiler 1.2.6 in embedded QuickJS, with a native C audio host. The old lists
below describe gaps in the **hand-written C statement parser**, not the new
JavaScript runtime. That parser remains available for the legacy line dialect.
See `docs/javascript.md` for tested coverage and the remaining native audio
work (specialized synths, stretching, convolution, and
browser-only integrations). Full WebAudio/SuperDirt sound parity is not done.

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
- [x] Song editor follows the playing step when the buffer is clean.
      Auto-advance never set the follow flag, so the editor stuck on
      one track while the audio moved on. Dirty edits are not
      overwritten (F5 commits). Needs a restart of the running UI.
- [x] Song list grows on the heap (exact-size strings). 32 × 64 KiB
      slots are gone. Safety ceiling `SONG_TRACK_CEILING` 8192.
      Each snapshot is the exact source. The editor grows the same way.
- [x] Mini-notation closer to https://strudel.cc/learn/mini-notation
      and the workshop recap: `!` replicate, `@` elongate, `/` slow
      across cycles, `?` degrade (seeded by cycle so lookahead matches),
      `bd(3,8)` / `bd(3,8,rot)`, `{a b, c}` polymeter, `|` choice
      *inside* a pattern. `|` before a transform stays the track bar.
- [x] Statement form from the workshop, one pattern per line:
      `setcpm`, `note` / `n` / `s` / `sound`, `.gain` `.lpf` `.pan`
      `.fast` `.slow` `.rev` `.attack` `.decay` `.sustain` `.release`
      `.scale` `.bank` `.room` `.delay` `.degrade` `.degradeBy`,
      and `slider(v)` / `slider(v,min,max)` / `slider(v,min,max,step)`.
      Patterned numbers (`gain("[.25 1]*4")`) query by time.
- [x] Widgets are not inline HTML. A slider bar sits on the code row,
      sliders list in the state pane, Ctrl+Left / Ctrl+Right nudge
      without rewriting the source. A one-row piano roll of the cursor
      line and a one-row scope sit on the bottom of the code pane when
      it is tall enough. F12 keeps the full roll.
- [x] Tests taken from strudel.cc workshop pages (simple through a
      multi-method line), in `tests/test_strudel.c`. `make test`
      (pattern, harmony, strudel) is ALL PASS.

### Historical legacy C parser roadmap

The unchecked items in this section describe the optional legacy C dialect.
They are not the current JavaScript runtime's product gap list. JavaScript
already uses upstream constructors, transforms, alignment, signals, multiline
statements and source locations; see `docs/javascript.md`. Keep this record
for work on the legacy parser, without treating it as unfinished JS support.

Goal: a line copied from strudel.cc should parse and do the same
musical thing. The parser stays hand-written. Do not generate it
from Peggy or Bison: a typo must keep a partial pattern plus
`pattern_last_error()`. Read the grammars below as the spec, then
grow `src/pattern.c` and `src/strudel.c` to match them.

Canonical references (the GitHub wiki is archived and stale; the
manual now lives on the site, and the repo moved to Codeberg):

- https://strudel.cc/technical-manual/repl/ — the pipeline. User
  code is JavaScript plus sugar, parsed with acorn. `"` and
  backticks become a mini-notation call. `'` stays a plain string.
  Bare names that look like notes (`c4`, `bb2`, `fs3`) become
  strings. The scheduler calls `Pattern.queryArc` on a time span
  and each hap is a bag of control parameters
  (`{ note, s, gain, cutoff }`), not a separate keyword per voice.
- https://strudel.cc/understand/cycles/ — `setcpm(x)` is
  `setcps(x/60)`. The usual 4-feel spelling is `setcpm(110/4)`.
  Our cycle is 4 beats, so BPM = cpm × 4, and `setcpm(120/4)` is
  120 BPM. That matches this page. Do not "fix" it to a 1-beat cycle.
- https://strudel.cc/learn/mini-notation/ and the PEG
  https://codeberg.org/uzu/strudel/src/branch/main/packages/mini/krill.pegjs
  (from krill, https://github.com/Mdashdotdashn/krill).
- https://strudel.cc/learn/code/ — calls, arguments, chaining.
- https://strudel.cc/learn/factories/ — `cat`/`seq`/`stack`/
  `stepcat` and the mini forms they correspond to.
- https://strudel.cc/workshop/recap/ — the short function list.
- https://strudel.cc/technical-manual/alignment/ — how two patterns
  combine: `in`, `out`, `mix`, `squeeze`, `squeezeout`, `reset`.
- https://strudel.cc/learn/mondo-notation/ — `#` chaining and the
  extra infix operators.
- https://codeberg.org/uzu/strudel — current source. Package map:
  https://strudel.cc/technical-manual/packages/

`setcpm()` is implemented and tested. `setcpm(45)` is BPM 180.
`setcpm(120/4)` is BPM 120. `setcps(x)` is BPM `x * 240`
(`x` cycles/second × 60 × 4 beats). Clamped to 20–400. The running
UI only has a change after it is restarted onto the new binary.

Shipped calls and methods:

- [x] `setcpm`, `setcps`
- [x] `note`, `n`, `s`, `sound`, and a leading `$:` label
- [x] `.fast` `.slow` `.rev` `.gain` `.lpf` / `.cutoff` `.pan`
- [x] `.attack` / `.att` `.decay` / `.dec` `.sustain` / `.sus`
      `.release` / `.rel`
- [x] `.scale` `.bank` `.s` / `.sound` (oscillator name or bank)
- [x] `.room` `.delay` `.degrade` `.degradeBy` `.sometimes`
- [x] `slider(value[, min, max[, step]])`
- [x] Arithmetic in numeric arguments: `+ - * / %` and parentheses.
      `setcpm(120/4)` is 30 cycles per minute, so BPM 120.
      `.lpf(200*4)` and `.gain(slider(1)/2)` use the same parser.
      A `/` inside quotes is still mini-notation slow, not division.
- [x] Mini-notation listed under **Now** (`* ! @ / ?`, postfix
      euclid, `{}`, in-pattern `|`)

Sugar the manual treats as part of the grammar, still open:

- [ ] `"` and backticks are mini-notation. `'` is a plain string
      (`.scale('C minor')` must not be parsed as a pattern).
      Backticks may span lines.
- [ ] Bare note names outside quotes (`c4`, `bb2`, `fs3`) are
      strings, as the transpiler does.
- [ ] Several top-level statements play together. `$:` marks a
      statement the REPL should play; two `$:` lines are parallel,
      which our one-track-per-line model already is once each line
      is active. Two statements on one line are not.
- [ ] A hap is one value with many controls. `.cutoff(sine.range
      (200,1000))` changes the filter of the same events, rather
      than a second track. Alignment (`in`/`squeeze`/…) applies
      when a control is itself a pattern.
- [ ] Mark the sounding step in the code row. Peggy stores
      `location()` on atoms; `patternifyAST` calls `withLoc`.
      We do not.

Mini-notation spec, read from current Codeberg `main` (2026-10-17):

- Raw grammar: https://codeberg.org/uzu/strudel/raw/branch/main/packages/mini/krill.pegjs
- Raw evaluator: https://codeberg.org/uzu/strudel/raw/branch/main/packages/mini/mini.mjs
  (`patternifyAST` / `applyOptions`). The HTML tree is a cookie
  interstitial; use the raw URLs.
- The musical meaning of `<>`, `*`, `/`, `!`, `@`, `?`, `|`, `~`
  is the learn page (https://strudel.cc/learn/mini-notation/).
  Where `mini.mjs` and that page disagree, follow the page and
  note the code. Do not generate a parser from the PEG.

What the PEG actually parses (quoted mini only; both `"` and `'`
are quotes *inside* krill — the “`'` is a plain string” rule is
the JS transpiler, not this grammar):

- Whitespace inside the quotes includes space, tab, CR, LF, and
  NBSP, so one mini string may span lines.
- A step is unicode letters plus `[0-9~]`, `-`, `#`, `.`, `^`,
  `_`. A step that is exactly `.` or `_` is not an atom (those
  characters are delimiters). `f#4` and `bb2` are one atom.
  A step that is exactly `~` or `-` is silence. A numeric atom
  becomes a number.
- Space = sequence (fastcat). `,` = stack. `|` = one choice per
  cycle (`chooseInWith`, seed incremented at parse time,
  `rand.early(0.0003*seed).segment(1)`).
- `[]` is a subcycle. `.` between slices (with a boundary the
  step does not swallow, so `a . b`, not `a.b`) is "feet":
  fastcat of those sequences, an alternative to `[]`.
- `<>` is `polymeter_slowcat`, not a coin-flip. The learn page
  says `<e5 b4 d5 c5>` is `[e5 b4 d5 c5]/4`: length follows the
  event count, and adding an event lengthens it. A comma inside
  `<>` stacks the parts (PR #886). Current `patternifyAST` slows
  each child by `child.__weight`, and `__weight` is set only for
  a weighted sequence; an unweighted sequence sets `_steps` and
  leaves `__weight` empty. That does not match the learn page.
  Our `<>` is `NODE_ALT` (one child per cycle). Implement the
  learn-page meaning.
- `{}` is polymeter: children are stacked after
  `fast(steps/weight)`. Default steps are the first child's
  weight. A trailing `%slice` (`{a b, c d e}%8`) is
  steps-per-cycle. `%` is not a general pace operator. `.pace()`
  is a separate function.
- A leading `^` on a sequence marks it stepwise
  (`__steps_source`). Stacks take the LCM of those step counts.
- Postfix operators bind left to right, and `*` `/` `()` `:` `..`
  take a slice, not only a number:
  - `@` or `_` sets weight `(old ?? 1) + (n ?? 2) - 1`. Bare `@`
    or `_` adds 1 (1 becomes 2). `@3` is 3. `@@` accumulates to 3.
    `_` must sit where the step will not eat it (`bd@3`, `bd _3`,
    `[a b]_`).
  - `!` is replicate: `_repeatCycles(n)._fast(n)`, and the weight
    becomes `n`, so the copies keep their duration and the slot
    grows. The same formula as weight: bare `!` adds 1, so `x!!!`
    is `x!4` and `x!!` is 3. Our `!` splices copies (flat
    `bd!3 sd` matches) but multiplies (`!!` becomes 4) and does
    not accumulate `@`.
  - `*slice` is fast, `/slice` is slow. `*2`, `*2.75`, `*[a b]`,
    `/<3 5>` are all legal. Our `*` `/` accept only a number.
  - `?` is degradeBy, default probability 0.5. Each `?` takes a
    new parse-time seed. Our `mix32(cycle, start, token)` is a
    different deterministic substitute.
  - `(p,s)` or `(p,s,r)` is euclid / euclidRot. `p`, `s`, and `r`
    are slices, so `<3 5>(5,8)` is in the grammar. Our postfix
    form accepts only integers, 1≤k≤n≤64.
  - `:` appends a value: `pat` paired with the tail, so `hh:1` is
    the sample name plus a number, not a string split. Repeated
    `:` grows the list.
  - `..` is an integer range, squeezed, then fastcat (`0..4`).
- Weighted sequences use timeCat (default weight 1). Unweighted
  sequences use `sequence` (fastcat).
- Parse errors are `[mini] parse error at line N`.

Experimental tail in the same PEG (`h()` only; the REPL uses
`m()`, which wraps the unquoted string in `"` and does not take
this path):

- `slow` / `fast` / `euclid` / `struct` / `rotL` / `rotR` /
  `scale` / `target`, written `op $ mini`.
- `cat [ a, b ]`.
- `setcps <number>`, `hush`, and `//` comments.
- `setbpm <number>` is `setcps(v/240)`, so `setbpm(120)` is 0.5
  cps. That is not our `setcpm`. Leave this tail until the quoted
  mini above is in place.

Shipped in `src/pattern.c` (hand-written, still partial on purpose):

- [x] `_` is weight when it is its own token (`bd _2`, `[a b]_`).
      `bd_2` stays one step. Bare `@` / `_` / `!` add one and
      accumulate: `@@` is 3, `!!` is 3, `!!!` is `!4`.
      `!` splices that many copies into the sequence, which is
      the learn-page result for `bd!3 sd` and `<bd!2 sd>`.
      A later `@` weighs each copy.
- [x] `*` and `/` take a group (`bd*[2 4]`, `/<2 4>`). A bare
      `*` is still `*2`, so `bd * sd` stays two bds and an sd.
      The factor is sampled inside the slot; each numeric piece
      speeds or slows the source there.
- [x] Postfix euclid arguments are slices. Constants still lower
      to a fixed grid (`bd(3,8)`). `bd(<3 5>,8)` is 3 hits, then
      5, using the number sounding at the start of the cycle.
- [x] `hh:1` joins into the event token. The sampler treats the
      number as a 0-based index among matches. A patterned tail
      (`hh:<0 1>`) relabels each overlap. `0..4` is the integers
      squeezed across the slot, capped at 64.
- [x] `{bd sd, cp}%4` sets the polymeter grid. `%` of a pattern
      is re-sampled each cycle. The grid defaults to the first
      layer's weight.
- [x] `<>` is elongation: `<bd sd>` is `[bd sd]/2`, and
      `<bd sd>*2` plays bd then sd in one cycle. A comma inside
      `<>` stacks the parts, each slowed by its own length.
- [x] `a . b` is feet (each side keeps its weights and they
      share the cycle). `a.b` stays one step. `^bd sd` parses;
      the mark does not change the rhythm. `~` and `-` are
      silence. A non-ASCII byte stays inside the step.

Not this slice: the experimental `h()` tail (`op $`, `setbpm`,
`hush`). `^` is stored for a later stepwise `pace`; it does not
yet take an LCM of a stack. `withLoc` highlighting is still open.

Still to implement, in roughly this order:

- [ ] Pattern constructors: `stack`, `cat` / `slowcat` / `seq`,
      `arrange`, `silence`, `pure`, `polyrhythm`. Several different
      calls on one line (`stack(s("bd"), note("c"))`) count here.
- [ ] Time warps: `.early` `.late` `.hurry` `.ply` `.palindrome`
      `.linger` `.zoom` `.compress` `.reset` `.segment` `.swing`
      `.swingBy` `.inside` `.outside` `.chunk` `.fastChunk`.
- [ ] Conditional structure: `.when` `.if` `.lastOf` `.firstOf`
      `.every` `.often` `.rarely` `.sometimesBy` `.someCycles`
      `.struct` `.mask` `.off` `.jux` `.juxBy` `.superimpose`
      `.echo` `.stut` `.iter`.
- [ ] Arithmetic on patterns: `.add` `.sub` `.mul` `.div` `.range`
      `.round` `.floor` `.ceil`. Mini euclid arguments that are
      patterns are shipped (`bd(<3 5>,8)`).
- [ ] Pitch: `.scaleTranspose` `.transpose` as methods (the `|`
      forms already exist). Chord voicings beyond the current ireal set.
- [ ] Sample controls: `.speed` (including `-1`), `.begin` `.end`
      `.loop` `.loopAt` `.cut` `.clip` `.unit` `.gain` already
      shipped. `samples(...)` / `samples('github:…')` / `samples(url)`
      load a map and fetch each file into `~/.cache/tompouce/web`
      the first time it plays (http/https only).
- [ ] Filters and color: `.hpf` `.bpf` `.lpq` `.resonance` `.vowel`
      `.crush` `.coarse` `.shape` `.postgain` `.distort`, FM
      (`.fm` `.fmdecay` `.fmenv`), phaser, tremolo.
- [ ] Delay extras: `.delaytime` `.delayfeedback`. `.roomsize`.
- [ ] Signals as arguments: `sine` `saw` `square` `tri` `rand`
      `irand` `perlin`, chained with `.range` `.seg` `.slow`.
- [ ] Mondo notation (`#`).
- [x] Dirt/Strudel sample *names* (`hh`, `sd`, `casio`, …) as a
      default map, not only `| bank` / `.bank()` against indexed dirs.
      Code is in `src/websamp.c` (side table, not `web_names`): local
      `Dirt-Samples/strudel.json` when present, then embedded uzu-drumkit
      so `hh:0`, `misc:2`, `bd`, `sd`, `mt`, `lt`, `ht` match current
      strudel.cc. `bank_find_dir` is an exact directory-component fallback
      (`banks_dir_named`). Installed `dough-samples/piano.json` and
      `tidal-drum-machines.json` now register default piano and machine
      names too. Vcsl and mridangam still require `samples()`.
      `tests/test_audio`,
      `tests/test_websamp`, and `tests/test_javascript` all pass, as does
      the full suite. See `AGENTS.md` "Continue here" for the fixed slice
      and stereo-effect bugs. The user restarted, and read-only MCP
      confirms the new samples and misc delay .5. Browser audio
      comparison remains open.
- [ ] Quoted patterns that span editor lines. Loading already joins
      an open quote; the editor is still one buffer, not a REPL cell.

### Later notes
- General MIDI program names are not a goal until the sample-name map is.
- OSC output is still deferred. ALSA `| midi CH` is shipped.

### Shipped earlier

## Near-term polish
- [x] Split pane resizing / adjustable console width. (F10/F11 split)
- [x] Fix top-of-screen flicker on small terminals. (no per-frame cls; full-width padded writes)
- [x] Add syntax highlighting in the editor. (colors + live error gutter)
- [x] Persist last BPM/code to `~/.config/tompouce/`. (state.json)
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
