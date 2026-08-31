---

title: Screenshots
description: HDR stills and recordings, plus example screenshot keybindings and capture workflows.

---

> **Note:** asteroidz has a compositor-native screenshot tool, `screenshot_ui`
> (region/window/screen capture, no external dependencies) — see
> [Keybindings](/docs/bindings/keys#screenshot_ui), and GPU capture
> below. The workflows in the rest of this page are upstream mango's
> external-utility approach (`grim`/`slurp`/etc.), still useful if you want
> something `screenshot_ui` doesn't do (e.g. annotation).

## GPU capture: HDR stills, and recording

Two dispatches encode the frame on the GPU, straight from the Vulkan
attachment AVK composited into — no capture client, no external tool, no
readback to 8 bits. Neither `grim` nor `screenshot_ui` can save what an HDR
output is actually showing; both hand you 8 bits, tone mapped.

```bash
amsg dispatch screenshot_hdr    # an HDR10 HEIF still of every output in HDR
amsg dispatch record_start      # start recording the focused output
amsg dispatch record_stop       # stop, and write the index
```

`screenshot_ui` opens the capture overlay — region, window or whole screen —
and the overlay itself says what it can do:

| key | action |
| :--- | :--- |
| `Enter` | save a PNG under `~/Pictures/Screenshots` (and copy it) |
| `C` | copy to the clipboard **only** — no file is left behind |
| `R` | record — the selection if you have dragged one, otherwise the whole screen; or stop the one already running |
| `Esc` | cancel |

Clipboard-only needs `wl-copy`: it writes a temporary PNG, hands it over, and
removes it in the same command. With no `wl-copy` on `PATH` the capture is
refused outright rather than quietly written to a file you did not ask for.

`R` after a drag records **just that rectangle**, and the overlay says so
before you press it. The encoder is built at the selection's size rather than
the screen's and cropped afterwards, so a small region costs a small region's
worth of GPU — which at 4K is the difference between fitting inside the frame
gap and not.

**The overlay never appears in what is captured.** A still is taken before the
overlay exists, and while the overlay is up the recorder captures nothing at
all — the recording has a gap across it, counted and reported at
`record_stop`.

`screenshot_hdr` writes one file per output currently running HDR, and refuses
one that is not: an SDR attachment is 8-bit, and the still would be claiming
BT.2100 PQ over a picture that is neither. Unlike `screenshot_ui,rawhdr`, which
writes the raw picture for inspection, this is a file an image viewer opens.

`azview` is that viewer, and it ships with the compositor:

```bash
azview ~/Pictures/asteroidz_DP-1_*.heic
```

It registers a desktop entry too, so a file manager offers it under **Open
With** for the formats it can actually decode — libheif's HEIF, HEIC and AVIF,
plus gdk-pixbuf's built-in loaders. WebP, JPEG XL, SVG and OpenEXR are
deliberately not claimed: gdk-pixbuf reads those only through loader modules
that ship separately, and offering to open a file and then refusing it is worse
than not offering.

It describes the surface the way the **file** describes itself — transfer
function, primaries, and the mastering display's own luminance — and lets the
compositor's HDR path run, rather than tone mapping the picture down to SDR
first the way a viewer that says nothing about its surface must. HEIF, AVIF,
PNG, JPEG and the rest of the common formats all open; an ordinary sRGB picture
is left untagged, which is what sRGB means on a Wayland surface.

The picture is scaled to fit the window without changing its proportions. The
**mouse wheel zooms** (10% a detent, and smoothly by fractions of one on a
touchpad), `0` returns to the fit, and `q` or `Escape` quits. Zoom crops the
viewport's source rectangle rather than growing the surface, so a magnified
picture stays inside the window, and the picture itself is uploaded once — a
36-megapixel photograph is 145 MB of texture, and re-sending it per wheel click
is the difference between a zoom that is smooth and one that is not.

`record_start` records the **focused** output to
`~/Videos/asteroidz_<output>_<timestamp>.mp4`, as **HDR10 from an HDR output
and sRGB from an SDR one** — it encodes what was composited, and both the
stream and the container say which it was. There is nothing to choose here: the
output decides.

Worth knowing before you rely on it:

- **The file is not playable until `record_stop`.** An MP4's index lives at the
  end, so a compositor killed mid-recording leaves a file nothing opens.
- **Capture is capped at 30fps** (`AZ_RECORD_FPS`), because a 4K picture is
  ~21ms of encode. Frames above the cap are skipped and counted; `record_stop`
  reports the split.
- **A fullscreen client that is being scanned out records nothing.** Nothing is
  composited, so there is no frame to encode, and the only sign is one line at
  `record_stop` saying the recording was discarded.

## External utilities

For the external-utility approach, compose your own workflow from small
Wayland utilities and bind them to keys:

| Tool | Purpose |
| :--- | :--- |
| [`grim`](https://github.com/emersion/grim) | Capture the screen or a region to a file |
| [`slurp`](https://github.com/emersion/slurp) | Interactively select a region for `grim` |
| [`wl-copy`](https://github.com/bugaevc/wl-clipboard) | Copy screenshots directly to the clipboard |
| [`satty`](https://github.com/gabm/Satty) | Annotate screenshots before saving |
| [`wayfreeze`](https://github.com/Jappie3/wayfreeze) | Freeze the screen before capture |

Install the required with your package manager or from source.

`grim` writes to the file path you give it, but **will not create missing directories**. Create one first:

```bash
mkdir -p ~/Pictures/Screenshots
```

Any directory works. `~/Pictures/Screenshots/` is just a convention.

## Quick Binds

Short, single-step commands can be placed directly in `config.kdl` with `spawn_shell`.
No script file needed.

### Fullscreen

Captures the entire display.

```kdl
binds {
    NONE+Print { spawn_shell "grim $HOME/Pictures/Screenshots/$(date +%Y%m%d%H%M%S).png"; }
}
```

### Region

Select an area with `slurp` before capturing.

```kdl
binds {
    Shift+Print { spawn_shell "g=$(slurp -d) && [ -n \"$g\" ] && grim -g \"$g\" $HOME/Pictures/Screenshots/$(date +%Y%m%d%H%M%S).png"; }
}
```

### Pointer

Captures the full screen including the cursor.

```kdl
binds {
    Alt+Print { spawn_shell "grim -c $HOME/Pictures/Screenshots/$(date +%Y%m%d%H%M%S).png"; }
}
```

### Clipboard

Captures to a temporary file and copies the image to the clipboard; no file is saved.

```kdl
binds {
    Ctrl+Print { spawn_shell "f=$(mktemp -t screenshot-XXXXXX.png) && grim \"$f\" && wl-copy < \"$f\" && rm -f \"$f\""; }
}
```

### Annotate

Captures and opens `satty` for drawing before saving.

```kdl
binds {
    Super+Print { spawn_shell "f=$HOME/Pictures/Screenshots/$(date +%Y%m%d%H%M%S).png && grim \"$f\" && satty --filename \"$f\" --output-filename \"$f\" --actions-on-enter save-to-file --early-exit"; }
}
```

## Script Binds

When a command involves multi-step logic, geometry parsing, FIFOs, or screen freezing,
move it into a script and invoke it with `spawn` instead of `spawn_shell`.

Create the scripts directory first:

```bash
mkdir -p ~/.config/asteroidz/scripts/screenshot
```

### Window

Uses `amsg` (ships with asteroidz) to capture the focused window.

`~/.config/asteroidz/scripts/screenshot/window.sh`:

```bash
#!/usr/bin/env bash
geometry=$(amsg get focused-client | jq -r '"\(.x),\(.y) \(.width)x\(.height)"')
[ -z "$geometry" ] && exit 1
grim -g "$geometry" "$HOME/Pictures/Screenshots/$(date +%Y%m%d%H%M%S).png"
```

```kdl
binds {
    Ctrl+Shift+Print { spawn "$HOME/.config/asteroidz/scripts/screenshot/window.sh"; }
}
```

### Freeze

Freezes the screen with `wayfreeze` before capturing.

`~/.config/asteroidz/scripts/screenshot/freeze.sh`:

```bash
#!/usr/bin/env bash
pipe=$(mktemp -u).fifo
mkfifo "$pipe"
wayfreeze --after-freeze-timeout 100 --after-freeze-cmd "echo > $pipe" &
wayfreeze_pid=$!
read -r < "$pipe"
grim "$HOME/Pictures/Screenshots/$(date +%Y%m%d%H%M%S).png"
kill "$wayfreeze_pid" 2>/dev/null
rm -f "$pipe"
```

```kdl
binds {
    Ctrl+Super+Print { spawn "$HOME/.config/asteroidz/scripts/screenshot/freeze.sh"; }
}
```

### Freeze + Region

Freeze, then select a region with `slurp`. Cleans up on cancel.

`~/.config/asteroidz/scripts/screenshot/freeze-region.sh`:

```bash
#!/usr/bin/env bash
pipe=$(mktemp -u).fifo
mkfifo "$pipe"
wayfreeze --after-freeze-timeout 100 --after-freeze-cmd "echo > $pipe" &
wayfreeze_pid=$!
read -r < "$pipe"
geometry=$(slurp -d)
if [[ -z "$geometry" ]]; then
  kill "$wayfreeze_pid" 2>/dev/null
  rm -f "$pipe"
  exit 1
fi
grim -g "$geometry" "$HOME/Pictures/Screenshots/$(date +%Y%m%d%H%M%S).png"
kill "$wayfreeze_pid" 2>/dev/null
rm -f "$pipe"
```

```kdl
binds {
    Shift+Super+Print { spawn "$HOME/.config/asteroidz/scripts/screenshot/freeze-region.sh"; }
}
```

Make all three scripts executable:

```bash
chmod +x ~/.config/asteroidz/scripts/screenshot/*.sh
```

## All-in-One Script

Prefer fewer files? A single script with subcommands covers every mode above.
Place it in the same directory and use it in place of the individual scripts.

`~/.config/asteroidz/scripts/screenshot/screenshot.sh`:

```bash
#!/usr/bin/env bash
set -euo pipefail
mkdir -p "$HOME/Pictures/Screenshots"
filepath="$HOME/Pictures/Screenshots/$(date +%Y%m%d%H%M%S).png"

case "${1:-fullscreen}" in
  region)
    g=$(slurp -d); [ -z "$g" ] && exit 1
    grim -g "$g" "$filepath" ;;
  window)
    g=$(amsg get focused-client | jq -r '"\(.x),\(.y) \(.width)x\(.height)"')
    [ -z "$g" ] && exit 1
    grim -g "$g" "$filepath" ;;
  freeze)
    p=$(mktemp -u).fifo; mkfifo "$p"
    wayfreeze --after-freeze-timeout 100 --after-freeze-cmd "echo > $p" & wp=$!
    read -r < "$p"; grim "$filepath"
    kill "$wp" 2>/dev/null; rm -f "$p" ;;
  freeze-region)
    p=$(mktemp -u).fifo; mkfifo "$p"
    wayfreeze --after-freeze-timeout 100 --after-freeze-cmd "echo > $p" & wp=$!
    read -r < "$p"; g=$(slurp -d)
    if [ -z "$g" ]; then kill "$wp" 2>/dev/null; rm -f "$p"; exit 1; fi
    grim -g "$g" "$filepath"
    kill "$wp" 2>/dev/null; rm -f "$p" ;;
  annotate)
    grim "$filepath"; satty --filename "$filepath" --output-filename "$filepath" --actions-on-enter save-to-file --early-exit ;;
  *) grim "$filepath" ;;
esac
```

Make the script executable:


```bash
chmod +x ~/.config/asteroidz/scripts/screenshot/screenshot.sh
```

Then add the binds to `config.kdl`:

```kdl
binds {
    NONE+Print { spawn "$HOME/.config/asteroidz/scripts/screenshot/screenshot.sh fullscreen"; }
    Shift+Print { spawn "$HOME/.config/asteroidz/scripts/screenshot/screenshot.sh region"; }
    Ctrl+Shift+Print { spawn "$HOME/.config/asteroidz/scripts/screenshot/screenshot.sh window"; }
    Ctrl+Super+Print { spawn "$HOME/.config/asteroidz/scripts/screenshot/screenshot.sh freeze"; }
    Shift+Super+Print { spawn "$HOME/.config/asteroidz/scripts/screenshot/screenshot.sh freeze-region"; }
    Super+Print { spawn "$HOME/.config/asteroidz/scripts/screenshot/screenshot.sh annotate"; }
}
```
