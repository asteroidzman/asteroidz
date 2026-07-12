---
title: FAQ
description: Frequently asked questions and troubleshooting.
---

### How do I arrange tiled windows with my mouse?

You can enable the `drag_tile_to_tile` option in your config. This allows you to drag a tiled window onto another to swap them.

```kdl
misc {
    drag-tile-to-tile 1
}
```

---

### Why is my background blurry or why does blur look wrong?

Blur applies to the transparent areas of windows. To disable it entirely, set `blur=0`.

If you are experiencing **performance issues with blur**, make sure `blur_optimized=1` (the default). This caches the wallpaper as the blur background, which is much cheaper on the GPU:

```kdl
effects {
    blur {
        optimized 1
    }
}
```

---

### Blur shows my wallpaper instead of the real background content

This is expected behavior when `blur_optimized=1` (the default). The optimizer caches the wallpaper to reduce GPU load — windows will blur against the wallpaper rather than the actual content stacked beneath them.

If you want blur to composite against the true background (i.e., show whatever is actually behind the window), set:

```kdl
effects {
    blur {
        optimized 0
    }
}
```

> **Warning:** Disabling `blur_optimized` significantly increases GPU consumption and may cause rendering lag, especially on lower-end hardware.

---

### My games are lagging or stuttering

Try enabling **SyncObj** timeline support.

```kdl
misc {
    syncobj 1
}
```

---

### My games have high input latency

You can enable **Tearing** (similar to VSync off).

First, enable it globally:

```kdl
misc {
    allow-tearing 1
}
```

Then force it for your specific game:

```kdl
window-rule { match title="Counter-Strike 2"; force_tearing 1 }
```

> **Warning:** Some graphics cards require setting `WLR_DRM_NO_ATOMIC=1` before asteroidz starts for tearing to work. Add it to `/etc/environment` and reboot, or launch asteroidz with `WLR_DRM_NO_ATOMIC=1 asteroidz`. See [Monitors — Tearing](/docs/configuration/monitors#tearing-game-mode) for details.

---

### How do I use pipes `|` in spawn commands?

The standard `spawn` command does not support shell pipes directly. You must use `spawn_shell` instead.

```kdl
binds {
    Super+P { spawn_shell "echo \"hello\" | rofi -dmenu"; }
}
```

---

### Certain key combinations do not work on my keyboard layout.

`bind` automatically converts keysym to keycode, which is compatible with most layouts but can sometimes be imprecise. If a key combination is not triggering, use the **keycode** directly instead of the key name.

Run `wev` and press the key to find its keycode, then use it in your bind:

```kdl
binds {
    Alt+q { kill_client; }
    Alt+code:24 { kill_client; }
}
```

You can also use `binds` (the `s` flag) to match by keysym instead of keycode:

```kdl
binds {
    Alt+q { kill_client; }
}
```
