---
title: Native Bar
description: The compositor's own built-in status bar.
---

asteroidz can draw its own status bar, with no external process and no
layer-shell client involved. It is built from the same pill widget as window
titlebars and monocle tab strips, so it inherits the [theme](./theming.md)
automatically and renders at each output's own scale.

This is **off by default**. It now covers what the Waybar setup it replaces
did — see [Scope](#scope) for what is still missing, which is the display
layout canvas and little else. The two can still run side by side.

## Enabling it

```kdl
bar {
    enable true
    height 48
    position "top"          // or "bottom"
    margin { x 8; y 9 }
    spacing 8
    pill-inset 6

    modules-left   "tags,layout,title"
    modules-center "media,clock,weather,idle"
    modules-right  "cpu,memory,network"

    clock { format "%H:%M:%S  ·  %a %d %b" }
}
```

| Key | Default | Meaning |
|---|---|---|
| `enable` | `false` | draw the bar at all |
| `height` | `48` | strip height in logical pixels (8–200) |
| `position` | `"top"` | `"top"` or `"bottom"` |
| `margin.x` | `8` | gap between the panels and the output's left/right edge |
| `margin.y` | `9` | gap between the panels and the output's top/bottom edge |
| `spacing` | `8` | gap between adjacent pills, applied only where a *chip* module (`tags`, `layout`) is on one side |
| `pill-min-width` | `28` | floor width, so single-glyph pills stay legible |
| `pill-inset` | `6` | vertical inset of the pill row inside the strip, so chips sit *in* the panel rather than spanning it |
| `pill-padding` | `6` | horizontal padding inside a status pill |
| `tag-padding` | `16` | horizontal padding inside a workspace/layout chip |
| `module-spacing` | `12` | separation between adjacent modules, measured **ink to ink** — each pill's own padding comes off it, so a label and a glyph sit the same distance apart as two glyphs |
| `tray-spacing` | `24` | the same, either side of the `tray`, which is other applications' icons rather than the compositor's own readouts |
| `volume-step` | `5` | percentage points the `volume` pill moves per scroll notch |
| `popover.width` | `340` | popover panel **minimum** width; it grows to its longest row, capped at half the output |
| `popover.row-height` | `34` | height of one popover row |
| `popover.spacing` | `2` | gap between rows |
| `popover.padding` | `12` | horizontal padding inside a row |
| `popover.gap` | `6` | distance from the bar's outer edge |
| `popover.color` | `0x0a0a0cf2` | popover fill (RGBA) |
| `min-tags` | `3` | pad the visible tag set up to this many with empty tags |
| `show-logo` | `true` | leading asteroidz ship pill on the workspace group |
| `tag-icons` | `3` | app icons drawn inside each tag pill (0 disables, max 4) |
| `show-all-tags` | `false` | `true` draws every configured tag; `false` draws only selected or occupied ones |
| `panel.enable` | `true` | draw a backdrop panel behind each non-empty slot |
| `panel.color` | `0x0a0a0cd9` | panel fill (RGBA; the default is ~85% opaque) |
| `panel.radius` | `9` | panel corner radius |
| `panel.padding` | `6` | **horizontal** inset between the panel edge and its pills |
| `panel.blur` | `true` | blur behind the panel (needs `effects.blur.enable`) |
| `panel.shadow` | `true` | drop shadow under the panel (needs `shadows`) |
| `modules-left` | `"tags,layout,title"` | comma-separated module list |
| `modules-center` | `"clock"` | " |
| `modules-right` | *(empty)* | " |
| `clock.format` | `"%H:%M:%S"` | `strftime` format |
| `media-width` | `280` | pinned width of the now-playing pill |
| `media.visualiser` | `true` | animate a spectrum in the media pill while playing |
| `media.bars` | `8` | spectrum bars (max 8) |
| `media.fps` | `20` | visualiser frame rate — **this is the cost dial**, see below |
| `weather.interval` | `15` | minutes between forecast fetches |
| `weather.location` | *(empty)* | city name; empty means IP geolocation |
| `interval` | `2` | seconds between `/proc` + `/sys` metric samples |
| `title-width` | `320` | **cap** on the title pill's width (`0` = uncapped); a shorter title gets a shorter pill |
| `icon-dir` | `<prefix>/share/asteroidz/bar-icons:~/.local/share:/usr/share` | colon-separated icon search path; our vendored art first, then Waybar plugin asset roots |

The three module lists share one budget of **24 modules total**, not 24 each.
Past it the parse stops and everything after that point is dropped, warning on
stderr — `asteroidz -p` will *not* catch it, because modules are parsed when a
bar is built per monitor rather than when the config is read.

Changes take effect on `reload_config` — including changes to the module
lists themselves, which rebuild the bars rather than just refreshing them.

An unknown module name is reported on stderr and skipped, so a config written
for a newer build still starts.

## Modules

| Name | Shows | Click |
|---|---|---|
| `tags` | the ship logo, then one chip per selected or occupied tag, labelled `N` or `N: name` (`tag N { name … }`, `set_tag_name`), followed by up to `tag-icons` app icons for what is running there | views that tag |
| `clock` | `strftime` of `clock.format` | — |
| `title` | the focused window's title, with its app icon | focuses that window |
| `layout` | the current layout, as its Waybar SVG | cycles `circle_layout` |
| `cpu` | total CPU load, from `/proc/stat` deltas — **icon only**, tinted by load | — |
| `memory` | used memory, from `/proc/meminfo` — **icon only**, tinted by load | — |
| `network` | two activity arrows — upload above, download below — each lit by its own throughput, from `/sys/class/net`. **Icon only** | — |
| `idle` | manual idle-inhibit state ("keep awake") | toggles it |
| `weather` | current temperature and condition, from open-meteo | — |
| `media` | now playing (title • artist), from MPRIS, with a live spectrum while playing. Shown only while **Playing or Paused** | play/pause |
| `volume` | default sink level, with a speaker icon (also accepts `vol`) | click toggles mute; right-click opens the output picker; scroll steps by `volume-step` |
| `notifications` | swaync's unread count and do-not-disturb state (also accepts `notify`) | click toggles the panel; right-click toggles DND |
| `medication` | doses due now, or the next dose's time (also accepts `meds`) | click lists today's doses; drill into one to take, skip or postpone it |
| `discord` | voice state from the `discord-voiced` daemon (also accepts `discord-voice`) | click opens the channel picker; right-click toggles mute |
| `vpn` | NordVPN state as a tinted shield (also accepts `nordvpn`). **Icon only** | click opens status + Quick Connect / Disconnect / countries |
| `display` | a monitor icon (also accepts `monitors`) | click lists every output; drill into one for HDR, resolution and scale |
| `tray` | one icon per StatusNotifierItem (also accepts `systray`) | left: `Activate`; right: the item's context menu; middle: `SecondaryActivate` |

The `tags` module mirrors the Waybar workspace module it replaces: it shows
every tag that is **selected or holds a window**, then pads with the
lowest-numbered empty tags up to `min-tags` (its `min-pills`). So a fresh
session shows three pills rather than one, and a nine-tag setup never spends
nine pills of width on tags holding nothing. `show-all-tags true` overrides
this with the dwm/dwl behaviour of always drawing every tag — but on a narrow
output those extra pills cost enough width to squeeze the centre section out.
An urgent tag is drawn in the theme's `urgent-color`.

## Fitting a narrow output

The module lists say what you *want*; the width available says what fits. When
the three sections cannot all fit, the bar sheds content in a fixed order
rather than letting whatever was laid out last run off the edge of the output:

1. **flexible pills shrink first** — the window title and the now-playing
   string ellipsise down to a floor before anything is dropped, since hiding
   the whole centre section because a title wanted 320px is the wrong trade.
2. **then whole modules are dropped, least important first**:
   `weather`, `media`, `medication`, `discord`, `title`, `idle`, `display`,
   `vpn`, `network`, `memory`, `cpu`, `notifications`, `volume`, `layout`,
   `clock`.
3. **`tags` and `tray` are never dropped.** Both are the bar's reason to
   exist — one is how you navigate, the other is how applications reach you.

The tray is what makes this necessary rather than merely tidy: its width is
however many applications happen to be running, so with a fixed layout an
unrelated program starting could push a module off the bar. A module that was
shed comes back on its own as soon as the width is there again (its nodes are
disabled, not destroyed).

## System tray

The `tray` module is a **StatusNotifierItem host**, on the same session bus the
compositor already pumps from its event loop.

Despite the name, `org.kde.StatusNotifierWatcher` needs **no part of KDE
installed or running**. It is a plain D-Bus name, and the compositor owns and
serves it itself; the `org.kde.` prefix is only historical, from KDE having
authored the spec before it went to freedesktop. It is what Steam, Discord,
Electron's AppIndicator, nm-applet, blueman, Syncthing and Nextcloud all
publish to. asteroidz additionally owns
`org.freedesktop.StatusNotifierWatcher`, the vendor-neutral spelling of the
same interface, exported from the same object.

The role is decided at startup:

- **Watcher** — we own the watcher name and applications register with us
  directly. This is the normal case.
- **Client** — another shell (a running waybar, say) already owns it. Taking
  the name away is not possible and queueing for it would leave the tray dead
  until that process exits, so asteroidz instead registers as a plain *host*
  with the incumbent watcher and mirrors its item list. Both bars then show
  the same tray, which is what you want while migrating from one to the other.

Icons come from `IconName` (resolved through the icon theme, and through the
item's own `IconThemePath` when it ships one — Electron apps and Steam name
icons that exist in no installed theme), falling back to the raw `IconPixmap`
pixels, which are decoded from wire-order ARGB32 into the shared icon cache.
`NeedsAttention` swaps in the attention artwork and fills the pill in the
theme's urgent colour. A `Passive` item is hidden: that status means "nothing
to say right now", and drawing it anyway is how a tray becomes a row of
identical grey squares.

Every call is async, so a wedged tray application cannot stall the compositor,
and an application that exits without unregistering is dropped on
`NameOwnerChanged` rather than leaving a dead pill behind.

**Not implemented: the DBusMenu context menu.** It needs a popup surface with a
keyboard grab and its own hit-testing, and the bar has no layer for that yet.
Right-click sends the item's own `SecondaryActivate`, which most applications
wire to "show my menu" regardless.

## Media visualiser

While something is playing, the media pill's glyph becomes a live spectrum,
driven by one long-lived `cava` in raw ASCII mode — the same source the Waybar
media plugin uses, read a line at a time rather than by forking per frame.

The bars are **mirrored**: each is centred on the glyph's middle and grows both
up and down, matching the Waybar visualiser's `mirror` mode. Grown from the
bottom instead, a quiet passage collapses everything onto the floor of its box,
so the glyph stops sitting on the same optical line as the text and icons
beside it and the pill reads as misaligned even though nothing moved.

**The FFT is not the cost.** cava's own analysis is a fraction of a percent of
a core; the cost is that an animating bar element damages the output every
frame, so the compositor recomposites at the animation rate for as long as
music plays. Three things keep that honest:

- cava only runs while something is *actually playing*, and is stopped the
  moment playback does;
- `media.fps` is deliberately low (20, not the display's rate);
- a frame whose bars have not moved past a small epsilon is skipped entirely,
  so a quiet passage costs nothing.

Set `media { visualiser false }` to keep the static transport glyph.

### When the pill is shown

Only while the followed player reports **Playing** or **Paused**. A player that
has finished lingers in MPRIS as `Stopped` — browsers especially — with its
last track's metadata still populated, so keying visibility off "we got some
metadata" leaves a stale `title • artist` on the bar with nothing playing
anywhere, sometimes for days. `Stopped`, and any other non-play state, hides
exactly like no player at all.

An already-followed player keeps the pill so it does not flap between two open
players — but a player that *is* playing takes it from one that is not.
Otherwise a browser tab sitting `Stopped` since login outranks the track you
just pressed play on.

### Which device it listens to

The monitor of the sink that is **`RUNNING`**, not the default sink. The
default is only where audio goes when nothing says otherwise — a player pointed
at a specific device plays somewhere else entirely. On a machine where the
default sink sat `IDLE` while music ran through the S/PDIF output, watching the
default's monitor showed a flat line through an entire track.

### When it cannot work

A player doing **bitstream passthrough** — AC3/DTS over S/PDIF, mpv's
`audio-spdif`, especially with `audio-exclusive=yes` — puts no PCM into the
graph at all and locks the device. There is nothing for cava, or any other
visualiser, to read. This is not a bug that can be fixed on this side; the
audio never exists in a form anything can analyse.

Rather than show six bars pinned at zero through a whole track, the pill
detects a few seconds of pure silence while the player reports playing and
falls back to its normal transport glyph. cava stays up, cheaply, so the bars
return by themselves if the signal does.

## Notifications

Unread count and do-not-disturb state from **swaync**, over the session bus.
The Waybar module this replaces runs `swaync-client -swb` as a long-lived
subprocess and shells out again on every click; swaync exposes all of it on
D-Bus, so `SubscribeV2` pushes every change and the toggles are plain method
calls. No polling, no subprocess.

The glyphs are the same Nerd Font set the Waybar config maps, including its
full six-state matrix. **Do-not-disturb and "inhibited" are different
conditions** — one means you asked for quiet, the other means something is
holding notifications back — and a bar that rendered them identically would be
misleading about whether messages are being dropped or merely held.

The count is drawn next to the glyph, which Waybar puts in a tooltip instead;
a bar pill has no tooltip, and "how many" is most of what the glyph is
consulted for. It is pinned to the width of a two-digit count so arriving
notifications never reflow the section.

Initial state comes from `NotificationCount` and `GetDnd` rather than
`GetSubscribeData`: that returns a bare `(bbub)` whose field order is not
self-describing, and reading it wrong would silently swap the count for the
DND flag.

## Medication

Reads the same store the Waybar medication plugin writes,
`$XDG_STATE_HOME/waybar-medication/medications.json`, and shows what that
plugin shows: the medication's **name** when exactly one dose is due, the
**count** when several are, the **next dose's time** when none is, and nothing
at all once the day is done. A due dose takes the theme's urgent colour, which
is what the plugin's pulsing red class conveyed.

**This module now owns the store.** It was read-only while Waybar still ran,
because two processes writing one JSON file with no locking is how a dose
record gets lost — and *"did I take it?"* is precisely the question this must
never get wrong. With Waybar gone, that hazard is gone with it.

Clicking the pill lists **today's doses**, each with its state, and drilling
into one offers **Take**, **Skip** and **Postpone**. Every dose is listed
rather than only what is due, because the question being answered is often
about a dose from this morning. The state marker *leads* each row: the popover
is a fixed width and rows ellipsise into it, so a status written after a long
medication name is the first thing lost.

Writes go through a temporary file and `rename()`, which is atomic on the same
filesystem — a reader sees the whole old document or the whole new one, never a
truncated file where a dose has no status. The records are written field for
field the way the plugin wrote them (`doseState` status/`takenAt`/
`postponedUntil`, a 200-entry `history`), because the file outlives the program
that made it.

The schedule is reproduced exactly rather than approximated, because a pill
that disagrees with the plugin about what is due is worse than no pill at all:

- `frequencyUnit "days"` — every `frequencyValue` days from `startDate`, at each
  listed `HH:MM`
- `frequencyUnit "hours"` — every `frequencyValue` hours from the first listed
  time, daily

A dose is **due** from its scheduled time until six hours later, unless its
`doseState` says taken or skipped. Only *today's* times are considered, so a
dose scheduled at `00:18` is missed rather than pending once that window has
passed — same as the plugin.

The store is re-read only when its mtime changes; between reads the due/pending
split still advances with the clock, so a dose coming due does not wait for a
file write.

## Discord voice

Voice state from the standalone **`discord-voiced`** daemon, over
`$XDG_RUNTIME_DIR/discord-voiced.sock`. The pill shows the Discord logo and
where you are — the joined channel's name, or `Idle`/`Connecting` — with a
mute glyph while self-muted and the theme accent while push-to-talk is held.
Clicking opens a picker of guild/channel rows with headcounts; a row joins,
and `Mute`/`Leave` sit at the top while connected.

**All Discord, audio and tokio work stays in the daemon.** This is a thin IPC
client exactly as the Waybar plugin is: newline-JSON events in, newline-JSON
commands out. Running songbird or an audio thread inside a *bar* has crashed a
session before; running it inside the **compositor** would take the whole
desktop with it.

With no daemon the module renders nothing at all — an always-present "Offline"
pill is noise on a machine that never runs it — and the socket client retries
with backoff up to 8s, so a daemon started later is picked up on its own.

The logo ships as a solid `#000` stencil and is tinted like the other
monochrome plugin artwork; painted as-is it is an invisible black blob on a
dark panel.

`src/common/unix-line-client.h` is the reusable half: a non-blocking
unix-socket line client on the compositor's event loop with backoff reconnect.
Distinct from `async-spawn.h`'s streaming mode, which owns a *child process* —
here the peer has its own lifetime and we are merely a client of it.

## VPN

NordVPN state from the `nordvpn` CLI, as a shield whose **colour is the
reading** — accent connected, amber connecting, dimmed disconnected, urgent
when the CLI or daemon cannot answer. Icon only, so it joins the tight
cpu/memory run; a server hostname is popover material, not bar material.

Polled rather than event-driven, because the CLI is all there is: nordvpn
exposes no bus interface and no socket to subscribe to. The poll is an async
spawn riding the shared metrics timer, so it costs one short-lived child every
`interval` seconds and never blocks — the Waybar plugin does the same for the
same reason.

The popover shows where you are connected (country, server, IP, uptime) as
inert rows, then Quick Connect or Disconnect, then a country list from
`nordvpn countries` that connects on click. Clicking a status row is consumed
without dismissing — it is information, not a target.

A missing binary shows the urgent shield rather than staying silent: the module
is opt-in, so someone who configured it without nordvpn installed should see
that plainly.

## Displays

Each bar reports **its own** output and mode, so a multi-head setup shows every
screen what it is running at rather than repeating the focused one. Clicking
lists every connected output; a row drills into that output's settings.

Being the compositor is the whole advantage here. The Waybar plugin shells out
to `amsg get all-monitors` for the topology and rewrites `monitors.kdl` plus a
`reload_config` to change anything. None of that applies: the monitor list is
`mons`, the fields are on `Monitor`, and a toggle is a field write and a
scheduled frame — no JSON, no subprocess, no round trip through our own IPC.

HDR is toggled per output, on the output the row names — deliberately not the
`toggle_hdr` dispatcher, which acts on the focused monitor by design; clicking
`HDMI-A-1` while `DP-1` is focused must not flip `DP-1`. The commit is deferred
to the next frame rather than issued inline, because an out-of-band commit
races an in-flight page-flip and gets rejected by the DRM backend — that is the
retrain-and-blank path. An output whose `hdr_capability_failed` is set says so
instead of offering a toggle that silently never takes.

The resolution and scale pickers, and SDR white, are covered — see
[Popovers](#popovers). **Not covered:** the layout canvas, which needs a
genuine drag surface rather than a row.

The per-output mastering, max-CLL and max-FALL values are deliberately not
offered either. Those describe what the *panel* can do — they are forwarded to
the display so its tone-mapper knows what it is being handed — so a control on
them would be inventing hardware facts, and an unset one has no honest value to
step away from. They stay output rules in the config, where a claim about your
hardware belongs.

## Popovers

A module can drop a panel below its pill holding rows the pointer can act on.

| module | opens |
|---|---|
| `volume` (right-click) | audio output picker, plus a volume stepper |
| `tray` (right-click) | that item's own context menu |
| `display` | every output → one output's settings → resolution / scale lists |
| `cpu`, `memory`, `network` | the system figures |
| `medication` | today's doses → take / skip / postpone |
| `vpn` | connection status and the country list |
| `discord` | voice channels, mute, leave |

Popovers are built from the pieces the bar already has — a scenefx
blur/rect/shadow stack for the panel, one pill node per row — so they inherit
the theme and the per-output scale for free and need no surface, no client and
no layer-shell round trip. A popover is just more scene nodes.

Exactly **one** is open at a time, session-wide. Two would need z-order
arbitration and a per-popover grab for no benefit: a bar popover is a menu, and
menus are modal by convention.

The panel is anchored to the centre of the pill that opened it and clamped to
the output, so a pill near the right edge — which is where the tray's would
always land — does not hang its popover off the screen. It sits below a top bar
and above a bottom one.

### Row kinds

A popover row is **one scene node**, so there is no sub-row hit testing — and
nothing is drawn that cannot be clicked. That constraint decides what a row can
honestly be:

| kind | behaviour |
|---|---|
| action | click runs it and closes |
| inert | a reading; consumed without dismissing, and drawn with no tile behind it — a filled row is what says "this is a target" |
| submenu | click re-opens the popover one level down |
| **stepper** | **scroll** over it to change its value in place |
| **choice** | click drills into a list of values, picking one applies it and returns |

A stepper is adjusted by scrolling because painting `−`/`+` zones would be
decoration that lies about where you may click. Scroll is already routed to
whatever node is under the pointer, so this costs no new input model. Clicking
a stepper is a miss rather than a selection, so it does not dismiss — reaching
for it should not punish you.

A **choice** row is the other half: a value whose valid set is arbitrary rather
than a range. A resolution cannot be a stepper — the modes are whatever the
panel says they are, so scrolling one would either invent modes or lie about
them. It reuses the submenu mechanism the tray menus already use, and the list
marks the current value and ends in a Back row.

The display popover's **Resolution** and **Scale** rows are the first two. The
scale list shows the logical size each scale produces, because "1.25" means
nothing on its own. Both are applied with `wlr_output_test_state` FIRST and
committed only if the test passes, so a picker can never black out a display;
a rejected commit is the retrain-and-blank path. The resolution row is hidden
on outputs with no mode list at all (every virtual backend: headless, nested
Wayland, X11).

The audio popover's `Volume` row is the first one: the number moves under the
pointer and the sink is set to that **absolute** percentage, not a relative
step, so it cannot drift away from the reading if something else changes the
volume meanwhile.

The display popover's `SDR white` row is the other stepper — the luminance an
SDR surface is mapped to inside an HDR output's pipeline, which is what decides
how bright the ordinary desktop looks once HDR is on. It sits on the *outputs*
panel rather than inside one output's own, because it is scene-wide, and it is
**hidden while no output is in HDR**: with none, it changes nothing you can
see, and a control that visibly does nothing is worse than an absent one. Its
rails are the ones `set_sdr_luminance` itself clamps to (80–1000 cd/m²), so the
row cannot show a value the dispatcher would refuse.

Steppers carrying a marked verb dispatch on the **verb**, not on the popover
kind, so one panel may hold more than one.

### System figures

`cpu`, `memory` and `network` all open the **same** panel — they are three
views of one machine, and three separate popovers of four rows each would be
filing rather than answering. The pills are numberless by design (the colour is
the reading); this is where the numbers they stand for live: CPU percentage and
load averages, memory and swap used against total, the three processes holding
the most resident memory, the interface with its rates and totals, and uptime.

Every row is inert — there is no honest action for "CPU 37%" — so the panel is
dismissed by <kbd>Escape</kbd> or a click outside it. It is sampled when it
**opens**, not on a timer: a panel that rewrites itself under the pointer is
hard to read, and these figures are being consulted rather than watched. The
pill beside it is the live one.

Processes are ranked by **RSS and not CPU**. A process's CPU share is a rate,
and one sample of `/proc/<pid>/stat` can only give its average since it started
— which for a browser open since breakfast is a number that looks like a
reading and is not one. RSS is a level, so a single sample is the truth.

Dismissal:

- a click on a row runs that row's action and closes;
- a click **anywhere else** closes, and that click is **swallowed** — a click
  that dismisses a menu must not also press whatever was underneath it;
- <kbd>Escape</kbd> closes.

### Keyboard

<kbd>↑</kbd>/<kbd>↓</kbd> walk the rows, <kbd>Enter</kbd> runs the one under the
cursor, <kbd>←</kbd>/<kbd>→</kbd> adjust a stepper, <kbd>PgUp</kbd>/<kbd>PgDn</kbd>
scroll, and <kbd>Escape</kbd> closes.

There is still **no keyboard grab** and the popover never takes focus. These
are handled in the compositor's own key path, ahead of the binding tables and
the focused client — exactly where <kbd>Escape</kbd> already was, which is what
showed a grab was never needed. Only those keys are taken; everything else
keeps working normally while a menu is up.

The cursor starts *nowhere*: a popover opened with the pointer should not
pre-arm <kbd>Enter</kbd> on a row nobody looked at, so the first arrow key
places it, and a click moves it to what was clicked. It skips separators and
readings — stopping on a row that cannot be acted on just makes <kbd>↓</kbd>
feel broken — and it wraps, because a menu is a ring.

The cursor is drawn as accent-coloured *text*, distinct from `selected`, which
fills a row to mean "this is the current value". The two answer different
questions and can point at different rows. A row that is both stays filled:
accent on accent would be invisible.

A popover whose content query returns nothing closes itself rather than leaving
an empty panel floating over the desktop — which is what happens on a machine
with no sound server, and is pinned by the regression suite.

### Tray context menus

A StatusNotifierItem's `Menu` property names an object implementing
`com.canonical.dbusmenu`, whose `GetLayout` returns a recursive tree. asteroidz
asks for **one level at a time**: a submenu is a row that re-opens the popover
against that row's id, which keeps the drawing flat and costs one round trip
per level entered rather than one enormous reply up front.

`AboutToShow` is called first, because applications populate their menus lazily
on it — skipping it gets an empty or stale layout out of anything Qt-based.
Labels have their GTK mnemonic markers stripped, so `_Quit` renders as `Quit`
rather than with a stray underscore. Separators are drawn as a dim rule and are
inert: clicking one neither acts nor dismisses, the way it behaves in every
other menu. Disabled entries are greyed and consume their click without closing.

A menu longer than the screen **scrolls**: the rows are all kept and a window
of them is drawn, so the wheel over the panel moves the list and
<kbd>PgUp</kbd>/<kbd>PgDn</kbd> jump a screenful. There is no clipping to do —
a row outside the window is simply never enabled. This used to truncate
instead, which quietly dropped whatever was at the bottom, and the bottom is
where the entry people reach for lives: Steam lists every installed game before
Store/Library/Community, and only then Quit. The row cap is deliberately
generous (32) for the same reason.

Items that publish no `Menu` still fall back to `SecondaryActivate` on
right-click, since some applications wire that to "open my menu" and ship no
DBusMenu at all.

## Panels

The bar has **no background of its own** — it is fully transparent. What you
see is one rounded translucent panel per non-empty slot, so the bar reads as
three floating groups rather than a full-width strip. That is the same shape as
the grouped panels in a typical Waybar config.

With panels on, a resting pill draws no background either; only the selected
tag (and any urgent one) is filled, so each panel reads as a single surface.
Turn panels off with `panel { enable false }` and every pill carries the
theme's resting colours instead, giving a row of separate pills.

The margin is measured to the *panel* edge, and panel padding is horizontal
only — the panel is exactly as tall as the bar strip, so the gap above and
below comes from `margin.y` alone. This mirrors the `margin: 9px 4px` /
`padding: 0 6px` of the Waybar groups it replaces.

Blur and shadow reuse the same scenefx nodes as the overview's top strip, and
each defers to the global `effects.blur.enable` / `shadows` settings — asking
for panel blur in a build with blur off costs nothing and draws nothing.

## Icons and fixed widths

Tag pills carry real **application icons** for the windows on that tag,
resolved through the same icon-theme lookup the titlebars use. Duplicate
app-ids collapse, so three terminals on one tag read as "terminals live here"
rather than filling the pill with the same glyph three times. Tag pills are
deliberately *not* width-pinned: a tag gaining a window is a real layout
change, not the per-tick jitter the pinning exists to suppress.

Pill icons are the **same SVGs the Waybar plugins use**, but **vendored** into
`assets/bar-icons/` and installed to `share/asteroidz/bar-icons`, which is the
first entry on the `icon-dir` search path. The bar replaces Waybar, so having
its appearance depend on which Waybar plugins happen to be installed was
backwards — and vendoring makes the art reviewable in-tree rather than living
only in `~/.local/share`.

The plugin trees stay on the path behind it (`~/.local/share`, then
`/usr/share`), so a locally customised plugin still wins for any file we do not
ship. `icon-dir` is a **search path**, not a single directory, because the
plugins do not share a prefix. A missing file means no icon, never an error.

**Every module draws artwork; none draws a font glyph.** Icons that had no
plugin equivalent — the notification bell in its three states, the idle
inhibitor's two — were authored for this bar and live under
`asteroidz-bar/`. Font glyphs cannot be held to a uniform size, which is
exactly what a row of pills needs.

The vendored art is normalised to **one 24×24 canvas with equal ink extent**
by `contrib/normalize-bar-icons.py`, because the upstream plugins never had to
agree with each other: viewBoxes of 24×24 and 100×100, ink filling the canvas
in one file and floating inside it in another. The bar draws every icon into
an identical square box, so those differences showed up directly as icons that
looked bigger or smaller than their neighbours. Run it with `--check` to
verify, and re-run it after adding art.

The status-plugin SVGs are **monochrome stencils** (a solid `#000`
silhouette), meant to be recoloured by the widget drawing them — painted as-is
they are an invisible black blob on a dark panel. The bar therefore paints its
tint *through* the icon's alpha, the same thing Waybar's `wb_themed_pixbuf`
does. Application icons and the ship logo are real artwork and are never
tinted.

`cpu` and `memory` are **icon only**: the colour is the reading, in four steps
— resting (foreground at 45% alpha), working (theme accent) from 20%, heavy
(amber) from 60%, saturated (theme urgent) from 85%. The exact figure is not
shown; that belongs in a popover.

`network` is icon only too, but its glyph is **drawn rather than loaded**: two
stacked arrows, upload above and download below, each lit independently by its
own direction's throughput on the same four-step ramp (bands at 8 KB/s,
512 KB/s and 4 MB/s). A tint is per-node, so one stencil cannot carry two
states — the artwork is rasterised into the shared icon cache instead, keyed on
the pair of tiers, so only the sixteen possible combinations are ever drawn
however long the session runs. A down link lights both arrows urgent; the pair
going red *is* the reading, so there is no separate disconnected glyph.

Icon-only pills carry no padding of their own and take no `pill-min-width`
floor (that floor exists to keep a single-glyph *label* readable, and on an
icon it only pads slack around the artwork).

### Spacing is measured ink to ink

`module-spacing` is a **separation, not a gap**: each pill's own horizontal
padding is subtracted from it, floored at 2px so two pills can never actually
touch. Chips are exempt — a chip draws a background, so its box edge is the
edge you see, and two chips are spaced box to box at `spacing`.

The reason is that a constant *box* gap renders as an inconsistent *visible*
one. Padding differs by kind — 0 for artwork, `pill-padding` for a label,
`tag-padding` for a chip — so 12px between boxes reads as 12px between two
icons, 18 between an icon and a label, and 24 between two labels. Three
further things pushed the same lie, all fixed by making a pill's extent equal
what you can see in it:

- **Icons cropped to their ink.** Artwork is fitted into a square by its long
  axis, so a glyph taller than it is wide left transparent margin either side
  that the eye read as spacing — a bell measured 16px from its neighbours
  where the cpu and memory icons beside it sat at 12. Loaded icons are trimmed
  to their alpha bounding box (as tray pixmaps already were) and advance by
  their real width rather than by the square.
- **Drawn icons fill their canvas.** The `network` arrows used to be inset 12%
  a side, which is the same margin drawn in by hand.
- **Reserves only for content that is present.** See below.

### Pinned widths

Every pill whose content changes shape is **pinned to the width of its widest
possible content**, so the bar never reflows and a pill never moves out from
under the pointer: throughput to `↓999.9M ↑999.9M`, the icon-only pills to
their artwork, and the clock to the widest rendering of its own `strftime`
format (probed once per month name, so `%a`/`%b` length variation is covered).
The title is the exception: `title-width` is a **cap**, not a pin, so a short
title takes a short pill.

A reserve is centred, so a pill reserving room for content it is not currently
showing carries the unused half of it as transparency on each side — which is
spacing, as far as the eye is concerned. Two modules are therefore pinned
*conditionally*:

- `notify` reserves for a two-digit count only while there **is** a count. An
  idle bell is exactly as wide as the bell; a notification arriving at 7 and
  dismissed at 70 still never reflows the section. Reserving unconditionally
  put ~13px of transparency either side of the module, which is what made the
  gaps around `volume`, `notify` and `display` read at twice everything else's.
- `volume` pins to the widest reading with the **same number of digits**
  (`8%`, `88%`, `100%`) rather than to `100%` outright. Digits are
  proportional, so an unpinned pill twitches on every step, while pinning to
  the maximum leaves a two-digit level floating in a hole a whole digit wide.
  The width is stable through a volume ramp and steps only at 9→10 and 99→100.

### When it does not all fit

The left and right slots are anchored to their edges; only the centre has
anywhere to go — and the whole point of the centre slot is that it is
*centred*, so it never yields first. Pills carrying ellipsisable text (the
title, the now-playing string) are marked flexible and give width back in two
passes: once if the three slots over-subscribe the output outright, and again
if the left slot reaches where the centred slot wants to start. A window title
therefore ellipsises as it approaches the clock rather than pushing the clock
off centre.

If the centre still cannot fit between its neighbours after that, it is
**hidden** rather than drawn through them — priority is workspaces, then
status, then the clock. A configuration that over-subscribes the width even
after that (a dozen pills on a 1000px output) will still overlap left against
right; there is nothing left to give.

## How it reserves space

The bar claims its strip from the output's usable area *before* layer-shell
surfaces are arranged. An external bar's exclusive zone therefore stacks
below it rather than fighting it, which is what lets you move modules across
one at a time while Waybar keeps the rest.

The footprint taken is `height + 2 × margin.y`.

## Building without it

The bar is a compile-time feature. With it off, none of the code reaches the
binary — no scene nodes, no timer, no config keys:

```sh
meson setup build -Dnative-bar=false
```

The `bar { }` config keys become unknown keys in that build, which the parser
warns about and ignores.

## Scope

Implemented: the bar frame, per-monitor layout in three panelled slots, click
routing, space reservation, shed-by-priority when the width runs out, every
module in the table above, and the popover layer they drop panels from.

`idle` reflects a **compositor-level** override, not the client-driven
`idle_inhibit_v1` protocol state — a bar module has no surface to attach a
protocol inhibitor to, the way Waybar's module does. The same override is
available as `amsg dispatch toggle_idle_inhibit` (append `,1`/`,0` to force a
state), so it can be bound to a key as well.

The metric modules read `/proc` and `/sys` directly, on a shared timer, once
per machine rather than once per monitor. That is deliberate and it is the
line between what is cheap to run in-compositor and what is not: the Waybar
plugins they replace fork `wpctl`/`nmcli`/`curl` on their main loop, which is
an invisible stutter in a bar process but a dropped frame and an input hitch
in a compositor.

`media` reads MPRIS over the session bus asteroidz already owns and pumps
from its event loop, with **no `playerctl` subprocess** — the bus answers
directly. Every call is asynchronous: `sd_bus_call()` blocks for up to its
25-second default timeout, and one unresponsive media player must never be
able to freeze the compositor. An already-followed player keeps priority over
newly-appearing ones so the pill does not flap between two open players.

`weather` is the first module that talks to the network. It shells out to
`curl` through an **async** helper (`src/common/async-spawn.h`): fork, keep the
read end of a pipe in the compositor's event loop, deliver the output in a
callback. It is never synchronous — a 3-second DNS stall on the compositor's
own event loop is a 3-second freeze of the whole session. Only one request is
in flight at a time, so a slow network cannot queue up a `curl` per tick. Same
open-meteo source and the same WMO-code→artwork mapping as the Waybar plugin,
so the pill is indistinguishable from it.

`notifications` needs swaync running; with no notification daemon it simply
shows its "nothing unread" glyph.

`tray` is a full StatusNotifierItem host — see [System tray](#system-tray) —
minus the DBusMenu context menu, which needs a popup layer that does not exist
yet.

`volume` is **event-driven**, which is the only way it belongs in a
compositor. One long-lived `pactl subscribe` reports every mixer change on
stdout, and each relevant event triggers a single asynchronous
`wpctl get-volume`; there is no poll and no fork per tick for state that
changes a few times an hour. `wpctl`/`pactl` rather than a libpulse or
libpipewire dependency: the round trip is off the event loop either way, and
linking a sound-server client into the compositor to render one glyph is not a
trade worth making.

With no sound server the subscriber exits immediately and the level never
arrives. Since the module is started from the refresh path — which runs on
every arrange — an unguarded restart would fork two processes per refresh, so
attempts are backed off to one every ten seconds. That still reconnects on its
own if pipewire is restarted underneath the session, and the regression suite
pins it by counting the compositor's children after a burst of dispatches.

Scroll is routed to whatever pill is under the pointer, and consumed there —
so scrolling the volume readout changes the volume even when the same wheel is
bound to switching tags. Both input families are handled, which matters more
than it sounds: a mouse wheel reports a discrete notch count, while a trackpad
or a high-resolution wheel reports only a stream of small continuous deltas
and no notch at all. Keying off the notch count alone leaves the pills dead
under a trackpad, so continuous motion is accumulated until it adds up to a
notch. The tray forwards scrolls to the item as the spec's
`Scroll(delta, orientation)`, letting a mixer applet take volume and a pager
take workspaces.

[Popovers](#popovers) carry the audio output picker, the tray's context menus,
the display controls, the system figures, the medication log, VPN and Discord
voice. Still missing: a draggable volume slider (that needs pointer capture
across a drag, not just a click), keyboard navigation within a popover, a
scrollable viewport for menus longer than the screen, and the display layout
canvas.

## Developing against it

A nested session is the safe way to iterate on the bar without restarting your
real one:

```sh
WLR_BACKENDS=wayland ./build/asteroidz -c /path/to/test.kdl
```

Give the test config no `output` block (or one with no `width`/`height`): a
nested output has no mode list, so it adopts the size of its host window and
follows it on resize. Naming an explicit mode there is likely to be rejected by
the backend, in which case the output falls back to the host window's size and
logs the rejection.

Do not point a nested instance at your real config — it would re-run every
`spawn-at-startup` entry into the live session.

## Cost

`bar_update` runs off the same internal broadcast that drives IPC event
watchers, which fires on every arrange — including each step of a window
drag. It hashes what the bar currently displays and returns early when
nothing changed, so an arrange that does not alter the bar costs one walk of
the client list and no redraw. The clock timer aligns to the next boundary
its format can actually show: a format without `%S` wakes once a minute, not
once a second.

A full `bar_update_all()` — every module on every monitor, re-measured and
re-laid-out — measures **~1.3ms median, 4.4ms worst case** with the whole
module set on one output (timed headless over 40 ticks). The clock re-arms to
the next boundary from wherever it actually ran, so it cannot drift; what it
cannot do is display a second the event loop was blocked through. A tick that
arrives late enough to have skipped one logs
`bar: clock tick Ns late (skipped N seconds)` at `WLR_INFO`, because from the
outside a blocked loop and a broken timer look identical — and at 1.3ms a
tick, a gap that size is always something else holding the loop.
