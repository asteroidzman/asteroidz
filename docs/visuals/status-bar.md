# The status bar

The bar is **not part of the compositor**. It lives in its own project,
[`asteroidz-bar`](https://github.com/asteroidzman/asteroidz-bar) — a quickshell
(QML/Qt6) shell that also owns the wallpaper.

It was drawn by the compositor for a while, because a bar client that forks
`wpctl` on its main loop is a stutter in a bar and a dropped frame in a
compositor. That trade was a bad one: it made every module the compositor's
problem — a plugin that hangs, a tray icon that decodes a 4096×4096 pixmap, a
menu that wants a text field. The modules that were expensive were made cheap
instead (`/proc` readers rather than subprocesses), and cheap modules are cheap
wherever they run. A client that misses a frame now misses only its own.

## What the compositor still does

Everything the bar cannot do for itself:

| | |
|---|---|
| `bar {}` and `theme {}` | resolved here — defaults, clamping, the matugen palette — and served over `get`/`watch bar-config` |
| `watch all-monitors` | tags, layout, focused title, per-output state |
| `set_output_*` | mode, scale, position, VRR, ICC, tested before they are committed |
| `ext-background-effect-v1` | the bar reports its panels' region and gets blur behind them, corners included. **Popups too**: a menu is an xdg popup, which is neither a toplevel nor a layer surface, so it used to be silently skipped -- see `popup_update_blur` |
| layer shell | the bar is an ordinary layer-shell client with an exclusive zone |

The palette is the interesting one. Handing a bar the config file to parse
would be two KDL readers that agree until one of them gains a default — and it
would still not see the palette, which matugen rewrites at runtime whenever the
wallpaper changes. The compositor is the only process that knows what the theme
currently *is*, so it serves the resolved answer and re-sends it on every
reload.

Build with `-Dbar-config=false` to compile that block and that IPC out
entirely, which is what a setup driving waybar or yambar exclusively wants.

## Running it

```kdl
spawn-at-startup "asteroidz-bar"
```

`bar { enable false }` is what tells it to start: the compositor answers
`amsg get bar-config` with the resolved block, and the launcher checks that
`.bar.enable` is explicitly `false` before starting the shell. That is a
leftover safety catch from the era when both existed and could be stacked, and
it costs nothing to keep.
