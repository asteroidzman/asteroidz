<div align="center">
  <img src="assets/asteroidz-256.png" alt="asteroidz logo" width="120"/>

  <h1>asteroidz</h1>

  <p>A fast, HDR-capable Wayland compositor for daily driving</p>

<a href="LICENSE"><img src="https://img.shields.io/badge/license-GPL--3.0-blue?style=flat" alt="License"/></a>

  <br/>
  <br/>

  <video src="https://github.com/user-attachments/assets/73407d39-d391-4743-826f-efc84492de28" width="720" controls muted playsinline></video>

</div>

---

asteroidz is a wlroots compositor with a dwm-style tag model and a modern
rendering pipeline. It aims to be lean and fast while shipping the things a
desktop actually needs: real HDR, tasteful effects, and window management
that stays out of your way.

## Highlights

- **HDR & 10-bit output** — BT.2020 + PQ signalling with a 3D-LUT color
  resolve pass, ICC profile support, EDID-derived luminance, and live
  `sdr_reference_luminance` / `sdr_saturation` controls so SDR content looks
  right on HDR panels
- **Dynamic VRR & tearing** — VRR that follows fullscreen games
  (`vrr_only_fullscreen` window rule) and content-type-aware tearing;
  video never tears, games can
- **Vulkan renderer** (default) — the full effect suite runs on the
  `asteroidz-scenefx` `fx_vk` renderer: blur (with pixel-accurate
  ext-background-effect-v1 regions), soft shadows, rounded corners, gradient
  borders, spring animation curves, plus HDR/SDR colour. GLES2 stays
  available as a fallback session
- **Privacy shield** — `shield_when_capture` window and layer rules cover
  marked surfaces whenever a screen-capture session is active
- **Tags, not workspaces** — per-tag layouts: scroller, master-stack,
  monocle (with icon pills), dwindle, grid, and more
- **Display resilience** — DPMS/disable monitor split, `retrain_monitor`
  and `dpms_wake_retrain` for panels whose DSC decoders wake up corrupted
- **The rest of a daily driver** — scratchpads, window swallowing, overview
  mode, hot-reload config, in-place restart, JSON IPC (`amsg`), gestures,
  GlobalShortcuts portal, xdg-toplevel icons, security-context filtering

## Building

asteroidz renders on **Vulkan by default** — the companion
[asteroidz-scenefx](https://github.com/asteroidzman/asteroidz-scenefx)
fork's `fx_vk` renderer (HDR10, rounded corners, blur, shadows, gradient
borders, SDR colour) — with **GLES2 as a fallback**. It's one binary; the
renderer is chosen per session via `WLR_RENDERER`.

Dependencies: wlroots 0.20, `asteroidz-scenefx` (built with `--prefix=/usr`
and both renderers), wayland, libinput, xkbcommon, pango/cairo, gdk-pixbuf,
cJSON, pcre2, libsystemd. The Vulkan renderer additionally needs the Vulkan
loader/headers and `glslang` (to compile the effect shaders to SPIR-V).

> The scenefx fork is renamed to **asteroidz-scenefx** — it installs as
> `libasteroidz-scenefx-0.5` / `asteroidz-scenefx-0.5.pc` and builds with the
> `gles2,vulkan` renderers, so it won't clash with an upstream `scenefx`.

A matching shell/bar is available as
[asteroidz-dms](https://github.com/asteroidzman/asteroidz-dms).

```bash
meson setup build --prefix=/usr
ninja -C build
sudo ninja -C build install
```

This installs the `asteroidz` binary, the `amsg` IPC tool, two wayland
session entries — **Asteroidz** (Vulkan) and **Asteroidz (GLES fallback)** —
and the GlobalShortcuts portal definition.

### Arch Linux

Everything asteroidz needs is in the official repos except the scenefx
fork, which you build from source. `wlroots0.20` lives in `extra`; the
stock `scenefx` packages are 0.3/0.4, so the renamed 0.5 fork
(`asteroidz-scenefx`) is a manual step.

Install the toolchain and dependencies (Vulkan loader/headers + `glslang`
are needed for the default Vulkan renderer):

```bash
sudo pacman -S --needed base-devel git meson ninja \
  wlroots0.20 wayland wayland-protocols libxkbcommon libinput \
  pcre2 pixman cjson pango gdk-pixbuf2 libdrm systemd-libs \
  vulkan-icd-loader vulkan-headers glslang \
  libxcb xcb-util-wm xorg-xwayland
```

Build and install the `asteroidz-scenefx` fork with both renderers, then
asteroidz:

```bash
git clone https://github.com/asteroidzman/asteroidz-scenefx.git
cd asteroidz-scenefx
meson setup build --prefix=/usr -Drenderers=gles2,vulkan
ninja -C build
sudo ninja -C build install
cd ..

git clone https://github.com/asteroidzman/asteroidz.git
cd asteroidz
meson setup build --prefix=/usr
ninja -C build
sudo ninja -C build install
```

Log out and pick **Asteroidz** (Vulkan) from your display manager's session
list, or **Asteroidz (GLES fallback)** for GLES2. From a TTY:
`dbus-run-session env WLR_RENDERER=vulkan asteroidz`.

(`xorg-xwayland` is only needed for X11 app support; drop it and build with
`-Dxwayland=disabled` for pure Wayland. Note: some native-Wayland GPU apps —
notably Electron — don't yet import on the Vulkan renderer and render blank;
run them under XWayland, or use the GLES fallback session.)

## Configuration

Config lives at `~/.config/asteroidz/config.kdl` (falling back to
`/etc/asteroidz/config.kdl`). Changes hot-reload with
`amsg dispatch reload_config`; `SUPER+CTRL+R` restarts in place without
ending the session.

See `docs/` for the full option and dispatcher reference.

## Acknowledgements

- [wlroots](https://gitlab.freedesktop.org/wlroots/wlroots) — Wayland protocol implementation
- [mango](https://github.com/mangowm/mango) — the compositor this project was forked from
- [dwl](https://codeberg.org/dwl/dwl) — the compositor family this project descends from
- [scenefx](https://github.com/wlrfx/scenefx) — the effects library our rendering fork builds on
- [niri](https://github.com/YaLTeR/niri) — inspiration for scrollable-tiling ergonomics
- [Hyprland](https://github.com/hyprwm/Hyprland) — inspiration for window-management UX
