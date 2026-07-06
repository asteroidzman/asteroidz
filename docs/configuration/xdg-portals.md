---
title: XDG Portals
description: Set up screen sharing, clipboard, keyring, and file pickers using XDG portals.
---

## Portal Configuration

You can customize portal settings via the following paths:

- **User Configuration (Priority):** `~/.config/xdg-desktop-portal/asteroidz-portals.conf`
- **System Fallback:** `/usr/share/xdg-desktop-portal/asteroidz-portals.conf`

> **Warning:** If you previously added `dbus-update-activation-environment --systemd WAYLAND_DISPLAY XDG_CURRENT_DESKTOP=wlroots` to your config, remove it. asteroidz now handles this automatically.

## Screen Sharing

To enable screen sharing (OBS, Discord, WebRTC), you need `xdg-desktop-portal-wlr`.

1. **Install Dependencies**

   `pipewire`, `pipewire-pulse`, `xdg-desktop-portal-wlr`

2. **Optional: Add to autostart**

   In some situations the portal may not start automatically. You can add this to your autostart script to ensure it launches:

   ```bash
   /usr/lib/xdg-desktop-portal-wlr &
   ```

3. **Restart your computer** to apply changes.

### Known Issues

- **Window screen sharing:** Some applications may have issues sharing individual windows. See [#184](https://github.com/mangowm/mango/pull/184) for workarounds.

- **Screen recording lag:** If you experience stuttering during screen recording, see [xdg-desktop-portal-wlr#351](https://github.com/emersion/xdg-desktop-portal-wlr/issues/351).

### 10-bit / HDR Screencasting

When a monitor is running with `hdr:1,bitdepth:10` (see [Monitors](./monitors.md)), asteroidz negotiates screencopy buffers in the output's real 10-bit render format (e.g. `XRGB2101010`) rather than clamping to 8-bit — this applies to `wlr-screencopy`, `wlr-export-dmabuf`, and the `ext-image-copy-capture` protocols alike, and carries through `xdg-desktop-portal-wlr`'s PipeWire stream since it negotiates dmabuf formats dynamically instead of assuming 8-bit.

Whether you end up with a genuinely 10-bit *file* depends on the capture tool's encoder settings, since most default to an 8-bit codec profile regardless of the input buffer depth. For example, with `wf-recorder`:

```bash
# Default settings silently downconvert to 8-bit H.264
wf-recorder -o DP-1 -f recording.mp4

# Force a real 10-bit output
wf-recorder -o DP-1 -x yuv420p10le -c libx265 -p profile=main10 -f recording.mp4
```

OBS Studio: set **Settings → Advanced → Color Format** to `P010` (or `I010`), and pick an encoder with a 10-bit profile (e.g. `libx265`/hardware HEVC/AV1 Main10).

**Caveat — no colorimetry passthrough:** when HDR is active, the composited buffer contains PQ (ST2084)-encoded samples, since asteroidz applies the PQ inverse-EOTF during rendering to drive the display. Neither `wlr-screencopy` nor `ext-image-copy-capture` transmit any colorimetry/transfer-function metadata alongside the captured frame — only raw pixel data and bit depth. A capture tool has no way to know the samples are PQ-encoded rather than plain gamma, so it will decode them as SDR/BT.709 by default, and recordings of bright/highlight content would look flat or washed out. This is a limitation of the upstream screencopy protocols (confirmed: no compositor, including KDE/KWin, ships a fix for this today), not something a compositor-side render change alone can fully resolve.

**Automatic fallback:** to avoid washed-out recordings, asteroidz automatically drops an output out of HDR for as long as an `ext-image-copy-capture` session (screenshot or screencast) is active on it, and restores HDR once capture ends. A short debounce (300ms) avoids a visible flash for quick screenshots. This means the *physical display* also visibly leaves HDR while something is recording it — not just the recorded file. Disable this with `hdr_capture_fallback=0` in your config if you'd rather keep true HDR on screen at all times (recordings will then look washed out per the caveat above). Note this only covers `ext-image-copy-capture` clients; legacy `wlr-screencopy`-only tools aren't covered since that protocol has no equivalent per-session signal.

The HDR/color-state change is folded into the output's next regular frame commit (not issued as a separate out-of-band commit) to avoid racing an in-flight page-flip. On some backends this can still occasionally fail a swapchain re-test; when that happens asteroidz falls back to the same mode-cycle "retrain" used elsewhere for HDR/DSC recovery, which can add up to ~1-1.5s of delay and a brief blink before the fallback fully lands, instead of applying near-instantly.

## Clipboard Manager

Use `cliphist` to manage clipboard history.

**Dependencies:** `wl-clipboard`, `cliphist`, `wl-clip-persist`

**Autostart Config:**

```bash
# Keep clipboard content after app closes
wl-clip-persist --clipboard regular --reconnect-tries 0 &

# Watch clipboard and store history
wl-paste --type text --watch cliphist store &
```

## GNOME Keyring

If you need to store passwords or secrets (e.g., for VS Code or Minecraft launchers), install `gnome-keyring`.

**Configuration:**

Add the following to `~/.config/xdg-desktop-portal/asteroidz-portals.conf`:

```ini
[preferred]
default=gtk
org.freedesktop.impl.portal.ScreenCast=wlr
org.freedesktop.impl.portal.Screenshot=wlr
org.freedesktop.impl.portal.Secret=gnome-keyring
org.freedesktop.impl.portal.Inhibit=none
```

## File Picker (File Selector)

**Dependencies:** `xdg-desktop-portal`, `xdg-desktop-portal-gtk`

Reboot your computer once to apply.