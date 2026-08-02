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

Everything the bar cannot do for itself — and nothing else. There was a `bar {}`
block here for a while: sixty-two values the compositor stored, clamped,
described over IPC and never once read, left behind when the drawing moved out.
They live in `~/.config/asteroidz-bar/config.kdl` now. A config this program
does not read is a config it should not own.

| | |
|---|---|
| `theme {}` | resolved here — defaults, clamping, the matugen palette — and served over `get`/`watch bar-config` |
| `watch all-monitors` | tags, layout, focused title, per-output state |
| `set_output_*` | mode, scale, position, VRR, ICC, tested before they are committed |
| `ext-background-effect-v1` | the bar reports its panels' region and gets blur behind them, corners included. **Popups too**: a menu is an xdg popup, which is neither a toplevel nor a layer surface, so it used to be silently skipped -- see `popup_update_blur` |
| layer shell | the bar is an ordinary layer-shell client with an exclusive zone |
| `assets/bar-icons` | the artwork, installed to `/usr/share/asteroidz/bar-icons`, which the bar searches by default |

The palette is the interesting one. Handing a bar the config file to parse
would be two KDL readers that agree until one of them gains a default — and it
would still not see the palette, which matugen rewrites at runtime whenever the
wallpaper changes. The compositor is the only process that knows what the theme
currently *is*, so it serves the resolved answer and re-sends it on every
reload.

Build with `-Dbar-config=false` to compile that block and that IPC out
entirely, which is what a setup driving waybar or yambar exclusively wants.

## Why the artwork lives here

The bar's icons are in the *compositor's* tree, which looks backwards until you
ask who installs them. They came from a dozen separate waybar plugin repos, so
which glyphs you had depended on which plugins you had installed, and a bar that
draws a blank pill because an unrelated package is missing is not a bar anyone
can debug. They were vendored into `assets/bar-icons` and normalised to one
canvas (`contrib/normalize-bar-icons.py`) so the set is complete, tracked, and
reviewable.

The bar's own `bar { icon-dir }` is a **search path**, tried in order, first hit
wins — the packaged directory last, so a locally-customised asset still beats
it. Unset, it searches the packaged directory, `~/.local/share` and
`/usr/share`, which is where these are installed.

Two of them are drawn rather than merely displayed: the ship logo's exhaust is
recoloured to the theme accent at runtime by the bar (a string substitution on
the SVG source, because tinting through the alpha channel would flood the hull
too), and the layout indicator is keyed on `layout_index` rather than the
symbol. Both are documented where they happen, in `asteroidz-bar`'s `Logo.qml`
and `Layout.qml`.

`meson test bar-icons` checks every one of them parses and rasterises to
something non-empty. A bar icon fails silently — a missing file and an
unparseable one are both just a blank pill — and both have shipped.

## Running it

```kdl
spawn-at-startup "asteroidz-bar"
```

Nothing gates it any more. There was a `bar { enable false }` flag the
autostart script read as "the compositor is not drawing one, so start the
shell" -- a guard against two bars stacking, from when this program drew one.
It cannot draw one now.
