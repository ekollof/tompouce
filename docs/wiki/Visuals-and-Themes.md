# Visuals and themes

## Inline visual feedback

Append a view to a pattern:

```js
note("c4 e4 g4 c5").s('triangle').gain(0.4)
  ._pianoroll()._scope()
```

Available kinds are `_scope`, `_pianoroll`, `_punchcard`, `_spiral`, `_pitchwheel`, `_spectrum` and `_wordfall`. `tscope` aliases scope; spellings without the underscore also work.

Adding, moving or changing supported literal visual options can update a view without F5 while the musical branch remains matched. Musical changes and options requiring JavaScript evaluation still need F5. A waiting view may indicate unmatched edited music.

Scope shows the branch's waveform; spectrum analyzes that isolated signal. Their audio feed is before shared orbit/master effects. Note views follow the final scheduled timeline, including downstream time transforms. Browser-only visual options are not all available.

Widgets scroll with source and do not become editable text. Current layout limits include 32 visual sites, one widget per kind per source line and eight visible Kitty placements; additional widgets use braille.

## Graphics and panels

Kitty-capable terminals get pixel graphics after successful capability detection. Other terminals, including direct tmux/screen sessions, retain braille. Select `/graphics braille` or restore `/graphics auto` through F2.

`/inline off` hides inline widgets; `/inline on` restores them. `/scope off` controls the bottom mini display separately. F12 toggles the independent large text visualization pane. These controls are distinct.

## Live themes

F2 accepts `/theme omarchy`, `/theme pywal`, `/theme auto` and `/theme off`. The selection persists. Auto prefers a valid Omarchy palette, then pywal; off restores startup colors.

| Source | Default file |
| --- | --- |
| Omarchy | `~/.local/state/omarchy/current/theme/colors.toml` |
| Older Omarchy fallback | `~/.config/omarchy/current/theme/colors.toml` |
| pywal | `~/.cache/wal/colors.json` |

XDG environment variables relocate these roots. Files are checked once per second, including atomic replacements and theme symlink changes. Invalid/missing palettes retain the last valid colors. Explicit pattern/visual colors take precedence; backgrounds preserve terminal transparency.

See [Troubleshooting](https://github.com/ekollof/tompouce/wiki/Troubleshooting) if graphics or colors do not update.
