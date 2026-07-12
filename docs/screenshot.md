---

title: Screenshots
description: Example screenshot keybindings and capture workflows for asteroidz.

---

> **Note:** asteroidz has a compositor-native screenshot tool, `screenshot_ui`
> (region/window/screen capture, no external dependencies) — see
> [Keybindings](/docs/bindings/keys#screenshot_ui). The workflows below are
> upstream mango's external-utility approach (`grim`/`slurp`/etc.), still
> useful if you want something `screenshot_ui` doesn't do (e.g. annotation).

For that external-utility approach, compose your own workflow from small
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
