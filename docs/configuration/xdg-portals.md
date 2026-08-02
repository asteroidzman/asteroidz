---
title: XDG Portals
description: Set up screen sharing, clipboard, keyring, and file pickers using XDG portals.
---

## Portal Configuration

You can customize portal settings via the following paths:

- **User Configuration (Priority):** `~/.config/xdg-desktop-portal/asteroidz-portals.conf`
- **System Fallback:** `/usr/share/xdg-desktop-portal/asteroidz-portals.conf`

> **Warning:** If you previously added `dbus-update-activation-environment --systemd WAYLAND_DISPLAY XDG_CURRENT_DESKTOP=wlroots` to your config, remove it. asteroidz now handles this automatically.

## Portals asteroidz Implements Itself

asteroidz is its own portal backend for two interfaces, served straight from
the compositor on `org.freedesktop.impl.portal.desktop.asteroidz`. Neither
needs a helper daemon installed, and both are selected by default in the
shipped `asteroidz-portals.conf`:

| Interface | What it gives you |
| --- | --- |
| `org.freedesktop.impl.portal.GlobalShortcuts` | Global shortcuts and push-to-talk for portal-aware apps (Discord, ashpd apps). A shortcut the app gives no usable trigger for raises a modal key-picker, and the pick persists in `~/.config/asteroidz/global-shortcuts`. |
| `org.freedesktop.impl.portal.Inhibit` | "Not now" — an app asking the screen not to blank, or the session not to end. |

### Inhibit

A sandboxed application has no Wayland surface to hang an `idle-inhibit-v1`
inhibitor on, so it asks over D-Bus instead. With no backend for that
interface, `xdg-desktop-portal` answers those requests with *"no such
interface"* and they silently do nothing — which is how a full-screen video
ends up blanking halfway through on a desktop where the same player's native
inhibitor would have worked.

The portal defines four things an app can ask to block. asteroidz enforces
two of them, records all four, and does not pretend about the rest:

| Flag | Asked for | What asteroidz does |
| --- | --- | --- |
| `1` | Logout | **Named in the exit prompt.** The compositor *is* the session — there is no session manager to veto an exit, and the person at the keyboard outranks a background request. What honouring this means is that the confirmation prompt says who asked, and why, instead of the unsaved editor dying quietly behind a prompt that said nothing. |
| `2` | User switch | Recorded and listed, not enforced — asteroidz has no user switching, so there is nothing to inhibit. |
| `4` | Suspend | **Held**, as idle inhibition. |
| `8` | Idle | **Held**, against `wlr-idle-notifier-v1` — exactly like a client's own idle-inhibitor, so blanking, locking and an idle daemon's suspend are all held off. |

Suspend is deliberately the same lever as idle rather than a logind sleep
block. Idle-driven suspend is what an app asking not to be suspended almost
always means, and it is the half a compositor is genuinely in charge of. A
logind inhibitor lock would also override your own `systemctl suspend` and
your laptop lid — so one leaked request from one crashed application would
turn closing the lid into a no-op, with nothing on screen to say why. What is
enforced is the part that goes away when the app does.

An inhibition ends when the application closes its request, when the portal it
came through drops off the bus (watched for directly, so a crashed
`xdg-desktop-portal` cannot leave the machine awake for the rest of the
session), or at shutdown.

**Seeing who is holding it.** An inhibition taken this way has no window to
point at, so `get idle` lists them:

```console
$ amsg get idle
{"inhibited":true,"manual":false,
 "portal":[{"app_id":"org.mpv.Player","reason":"Playing video","flags":8,
            "logout":false,"user_switch":false,"suspend":false,"idle":true}]}
```

`inhibited` is the honest answer to "will this machine sleep"; an entry that
appears in `portal` without raising it is one asteroidz recorded and does not
enforce. `watch idle` pushes on any change to either, including a change that
only moves the list. See [IPC](../ipc.md).

**Monitoring the session state.** `CreateMonitor` sessions receive
`StateChanged` with `screensaver-active` (tracking the `ext-session-lock`
state) and `session-state` — Running, Query End while the exit prompt is up,
and Ending at shutdown. `QueryEndResponse` is accepted and logged, but nothing
waits on it: Query End here means a person is looking at the exit prompt, and
the exit waits for their keypress, not for a bus round trip.

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

**No automatic fallback, deliberately.** asteroidz used to drop an output out of
HDR for as long as an `ext-image-copy-capture` session was active on it, behind
an `hdr_capture_fallback` option. That has been removed: it fixed the recorded
file by changing the *physical display*, so every capture flashed the screen and
cost two modesets — and when the commit fell back to a retrain (see below), up to
~1–1.5s. The option no longer exists; a config that still sets it gets an
unknown-key warning.

What to do instead: capture in HDR and tonemap afterwards, or use the built-in
`screenshot_ui`, which reads the composited buffer back directly and tonemaps PQ
to sRGB in software, so it never touches the output's live HDR state.

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
org.freedesktop.impl.portal.Inhibit=asteroidz
org.freedesktop.impl.portal.GlobalShortcuts=asteroidz
```

A user config **replaces** the shipped one rather than merging with it, so
copy the two `asteroidz` lines across when you write your own — leaving them
out silently gives back the "no such interface" behaviour they exist to fix.

## File Picker (File Selector)

**Dependencies:** `xdg-desktop-portal`, `xdg-desktop-portal-gtk`

Reboot your computer once to apply.