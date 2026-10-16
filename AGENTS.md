# AGENTS.md

Project: **StrudelTerm** – a terminal live-coding environment for music in C, with MCP support.

## Build / run
- Build: `make`
- Clean/rebuild: `make clean && make`
- Run interactive UI: `./strudelterm`
- Run MCP server over stdio: `./strudelterm --mcp`
- Run MCP server with terminal UI: `./strudelterm --mcp --ui`
- Run MCP over TCP (local only): `./strudelterm --mcp-port 18789`

Requirements (system packages): `slang`, `portaudio-2.0`, `libcurl`, `libcjson`, `leveldb`, `alsa`.

## Architecture
- `src/common.c`: common helpers, console ring buffer, timing.
- `src/audio.c`: PortAudio real-time audio, 128-voice pool, synth oscillators and drum voices. ALSA chatter silenced (shares the terminal with S-Lang).
- `src/harmony.c`: Strudel-style chords, scales, and scale degrees (`| scale`, `chord`, comma stacks).
- `src/pattern.c`: mini-notation lexer + forgiving recursive-descent parser
  (tokens with columns; partial results + `pattern_last_error()` on typos;
  deliberately not bison: LALR all-or-nothing errors are wrong for live
  coding) and pattern expansion.
- `src/track.c`: code parser (bpm, drum, notes, bass, lead), turns source into `Track`s.
- `src/scheduler.c`: cycle-based scheduler; generator thread looks ahead, consumer triggers audio. Also song list (buffer snapshots, per-cycle advance, lazy bank resolution).
- `src/ai.c`: OpenAI-compatible chat client (env: `OPENAI_API_KEY`, `OPENAI_BASE_URL`, `OPENAI_MODEL`). Note: when an API key is set, the prompt **and current code** are sent to that endpoint — document/limit egress; falls back to offline templates if curl init fails or no key is set.
- `src/wavetable.c`: procedural bandlimited wavetables (`inst wt:<name>`; tables sine tri saw square organ acid voice pluck pad glass).
- `src/banks.c`: background index of playable files under `assets/banks` (wav/sf2/sf3/sfz, symlinks skipped), persisted in LevelDB (`~/.cache/strudelterm/banks.ldb`, per-dir mtime validation).
- `src/sampler.c`: one-shot sampler (WAV/MP3/FLAC via vendored `thirdparty/`
  minimp3 + dr_flac; `| bank NAME`; alias + slot mapping, 44.1k mono float
  cache; melodic banks pitch-match by filename note, fractional playback,
  duration gating).
- `src/midi.c`: ALSA sequencer MIDI out (`| midi CH`), own off-queue worker.
- `src/tui.c`: S-Lang user interface (editor, console, AI prompt, piano-roll/scope viz).
- `src/mcp.c`: MCP JSON-RPC server + UI orchestration.
- `src/songfile.c`: whole-song JSON save/load (v2 `tracks`+`active`; v1 fallback), shared by TUI pickers/`/save`/`/load` and MCP `music_session`.

## Code style
- Keep functions short and single-purpose; prefer `strtok_r` (not `strtok`) for reentrancy.
- All shared state protected by mutexes; audio callback locks `voice_lock`; tracks lock `tracks_lock`.
- Never block in the audio callback; use lock-held sections sparingly.
- Keep UI state in `tui.c`, audio state in `audio.c`, etc.; don’t reach across modules unexpectedly.

## Secure coding practices (mandatory)
- **Never** use unbounded copies: prefer `snprintf`, `strncpy` with explicit size + NUL terminator, or `memcpy` with verified lengths. Ban `strcpy`, `strcat`, `sprintf`, `gets`.
- Treat all external input (MCP JSON, AI responses, terminal input, env vars) as untrusted: validate types, lengths, and ranges before use.
- Always bounds-check array/string writes (`code_lines`, `token[64]`, `out[65536]`). Never trust a length from a peer.
- Check every allocation and return value; handle `NULL`/failure paths.
- Use `strtok_r`, not `strtok`, in reentrant/multi-layer code.
- Keep object-size limits in mind for `snprintf`/`strncat`: always pass the *remaining* size, and verify termination.
- Do not shell out, `exec`, or invoke `system()` on strings derived from agent/LLM/terminal input.
- Do not write files based on untrusted paths; if persistence is added, sanitize and confine to a config dir.
- Keep the audio callback and scheduler lock windows short; avoid blocking syscalls there.
- Do not add backdoors/debug hooks (e.g., external process injection, hidden control channels) to production paths.
- Prefer `sig_atomic_t`/atomics for signal-shared flags; avoid non-reentrant calls in signal handlers.
- Run `make` cleanly under `-Wall -Wextra`; treat new warnings as errors to review.
  (`thirdparty/` vendored decoders are pragma-exempt: not our code.)

## Coding instructions for agents
- Preserve build cleanliness (`-Wall -Wextra`). Avoid new warnings.
- Test changes with `bash -n`-style sanity: rebuild and run `./strudelterm` briefly under a PTY if possible.
- Do not use `strtok` in multi-layer parsers; use `strtok_r`.
- Avoid static storage changes that break reentrancy/concurrency.
- When adding UI behaviour, always update `tui_frame()` and keep cursor logic intact.
- MCP tools should always return simple JSON text content; never print extra non-JSON to stdout in `--mcp` mode.
- Sensible defaults: don’t break headless mode, `/dev/tty` UI, or interactive keys.
- Keep source code ANSI C compatible (C11/C17); GCC is used.

## Common pitfalls
- `PortAudio` init errors are expected in constrained environments; tests can still pass after init.
- `SLang` and `SLsmg` calls must be called from the foreground/UI thread.
- `isatty(0)` is used to distinguish interactive launch from MCP agent pipe.
- `current_code[]` is the canonical source string for MCP tools.

## Extending
- Add operations in `track_load_from_code()` and update parser tokens carefully.
- Add UI keys in `tui_run()`.
- Add MCP tools in `mcp.c` `handle_request()` and mention them in tools/list.

## MCP interface (for agents/harnesses)

### Launch
- Headless: `./strudelterm --mcp` (JSON-RPC 2.0 over stdio, newline-delimited).
- With UI: `./strudelterm --mcp --ui`. UI displays on `/dev/tty`. When stdin is a pipe, JSON stays on that pipe. When stdin is a terminal, MCP listens on `127.0.0.1:18789` (override with `--mcp-port`). Loopback only, no auth.

### Protocol methods
- `initialize`
- `ping`
- `tools/list`
- `tools/call`
- `resources/list` / `resources/read` (`strudel://code`)

### Tools
| Tool | Arguments | Notes |
|---|---|---|
| `music_set_code` | `{code: string}` | Replace & evaluate source. |
| `music_evaluate` | `{}` | Re-evaluate current code. |
| `music_play` | `{}` | Start playback. |
| `music_stop` | `{}` | Stop playback. |
| `music_set_bpm` | `{bpm: number}` | Tempo. |
| `music_get_status` | `{}` | Returns bpm, tracks, playing flag, etc. |
| `music_get_code` | `{}` | Current source. |
| `music_get_console` | `{lines?: number}` | Recent logs. |
| `music_ai` | `{prompt: string}` | Generates code via LLM, loads/evaluates. |
| `music_song` | `{enabled?: bool}` | Song mode: play track buffers in sequence, one per cycle. |
| `music_track` | `{action: copy\|next\|prev\|hold\|list\|goto\|move\|remove, index?: number, to?: number}` | Song track list ops (copy snapshots playing code; goto family follows editor). |
| `music_session` | `{action: save\|load\|list\|delete, name?: string}` | Sanitized-name sessions in `~/.config` (no raw paths). |
| `music_health` | `{}` | Engine diagnostics line. |

### Example flow
```
1. tools/call music_set_code {code: "bpm 130\ndrum bd~bd~\nnotes c4 e4 g4"}
2. tools/call music_play {}
3. tools/call music_get_status {}
```

### Implementation notes
- All tool handles should update `current_code[]` when code changes.
- Maintain console logs with `log_console()` so agents can observe state.
- Do not print non-JSON to stdout in `--mcp` mode.
- Keep MCP server alive until EOF (blocking loop).
- No MCP tools for save/load: file paths from external agents are untrusted;
  agents transfer compositions via `music_set_code` / `music_get_code`.