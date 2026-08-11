---
title: Installation
description: Build asteroidz from source, or install its NixOS/Home Manager module.
---

> **Note:** asteroidz is a personal fork of [mango](https://github.com/mangowm/mango) and
> isn't packaged on any distribution — there's no AerynOS/AUR/Fedora/Gentoo/Guix/PikaOS
> package for it. If you want a distro-packaged install and don't need anything
> asteroidz-specific, those channels exist for upstream mango instead. To run
> asteroidz itself, build it from source or use its Nix flake below.

---

### NixOS

The repository provides a Flake with a NixOS module.

1. **Add flake input**

   ```nix
   # flake.nix
   {
     inputs = {
       nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
       asteroidz = {
         url = "github:asteroidzman/asteroidz";
         inputs.nixpkgs.follows = "nixpkgs";
       };
       # other inputs ...
     };
   }
   ```

2. **Import the NixOS module**

   **Option A — Import in `configuration.nix`:**

   ```nix
   # configuration.nix (or any other file that you import)
   {inputs, ...}: {
     imports = [
       inputs.asteroidz.nixosModules.asteroidz
       # .. other imports ...
     ];

     # ...
   }
   ```

   **Option B — Import directly in flake:**

   ```nix
   # flake.nix
   {
     # ...

     outputs = { self, nixpkgs, asteroidz, ...}@inputs: let
       inherit (nixpkgs) lib;
       # ...
     in {
       nixosConfigurations.YourHostName = lib.nixosSystem {
         modules = [
           asteroidz.nixosModules.asteroidz # or inputs.asteroidz.nixosModules.asteroidz
           # other imports ...
         ];
       };
     }
   }
   ```

   > **Note:** the flake input alias above (`asteroidz`) is just your own name
   > for the input and can be anything; the module attribute
   > (`nixosModules.asteroidz`) and option (`programs.asteroidz`) are fixed.

3. **Enable the module**

   ```nix
   # configuration.nix (or any other file that you import)
   {
     programs.asteroidz.enable = true;
   }
   ```

4. **Start asteroidz on login**

   The following are common examples. Refer to the official NixOS documentation for full configuration options.

   **Option A — greetd:** Autologin on first start; login screen after logout.

   ```nix
   services.greetd = {
     enable = true;
     settings = {
       initial_session = {
         command = "asteroidz";
         user = "your-username"; # auto-login on first start, no password required
       };
       default_session = {
         command = "${pkgs.greetd.tuigreet}/bin/tuigreet --cmd asteroidz";
         user = "greeter";
       };
     };
   };
   ```

   **Option B — Display manager autologin:** Autologin via an existing display manager (e.g. SDDM, GDM). [`addLoginEntry`](/docs/nix-options#addloginentry) (default: `true`) automatically registers asteroidz as a session.

   ```nix
   services.displayManager = {
     defaultSession = "asteroidz"; # derived from asteroidz.desktop filename
     autoLogin = {
       enable = true;
       user = "your-username";
     };
   };
   ```

   **Option C — getty autologin:** No login screen, boots directly into asteroidz on TTY1.

   For bash/zsh:

   ```nix
   services.getty.autologinUser = "your-username";

   environment.loginShellInit = ''
     [ "$(tty)" = /dev/tty1 ] && exec asteroidz
   '';
   ```

   For fish:

   ```nix
   services.getty.autologinUser = "your-username";

   programs.fish.loginShellInit = ''
     if test (tty) = /dev/tty1
         exec asteroidz
     end
   '';
   ```

5. **All available options**

   See [Nix Module Options](/docs/nix-options) for the full list of NixOS and Home Manager options.

---

## Ubuntu 26.04

`contrib/install-ubuntu.sh` does the whole thing — dependencies, wlroots, the
compositor and the bar — and is verified against a clean `ubuntu:26.04` container.

```bash
git clone https://github.com/asteroidzman/asteroidz.git
bash asteroidz/contrib/install-ubuntu.sh
```

Everything asteroidz needs is in the Ubuntu archive **except wlroots**: 26.04
ships 0.19 and asteroidz needs 0.20.2, so the script builds it into `/usr/local`,
where it sits beside the packaged 0.19 rather than replacing it — different
soname, different pkg-config name, so anything else wanting 0.19 keeps working.

The bar additionally needs **quickshell**, which is not in the archive. Add
whichever quickshell PPA you use before running the script; if it is missing the
script says so and carries on, since the compositor does not need it and the bar
still builds — it just cannot run until quickshell is there.

The script is re-runnable: each step skips itself if already done, so a failure
part-way can be fixed and the script run again. `ASTEROIDZ_TAG`, `BAR_TAG`,
`WLROOTS_TAG`, `PREFIX` and `SRC` are all overridable.

> Because wlroots is built rather than packaged, an Ubuntu update that changes
> `libwayland` or `libinput` can leave it stale. If asteroidz stops starting after
> an update, rebuild wlroots from `~/src/wlroots`.

## Building from Source

> **Info:** Ensure the following dependencies are installed before proceeding:
>
> - `wayland`
> - `wayland-protocols`
> - `libinput`
> - `libdrm`
> - `libxkbcommon`
> - `pixman`
> - `libdisplay-info`
> - `libliftoff`
> - `hwdata`
> - `seatd`
> - `pcre2`
> - `pango`
> - `cjson`
> - `pixman`
> - `xorg-xwayland`
> - `libxcb`
> - `libsystemd`
> - `gdk-pixbuf`
> - `vulkan-icd-loader`, `vulkan-headers`, `glslang` (for the experimental Vulkan renderer)

You will need to build `wlroots` and asteroidz's `scenefx` fork manually as well.

1. **Build wlroots**
   asteroidz currently tracks wlroots 0.20 (check `meson.build` for the exact
   required version).

   ```bash
   git clone -b 0.20.2 https://gitlab.freedesktop.org/wlroots/wlroots.git
   cd wlroots
   meson setup build --prefix=/usr \
     -Dxwayland=enabled -Dcolor-management=enabled
   sudo ninja -C build install
   ```

   **0.20.2, not 0.20.0.** `asteroidz-scenefx` reads
   `wlr_surface_output.suspended`, which arrived after the `.0` release — against
   0.20.0 the build gets a hundred files in and then fails on a struct member.
   `meson.build` requires `>=0.20.2` so this is caught at configure time now.

   **Both flags matter and neither is on by default.** `color-management`
   requires `lcms2` and asteroidz uses `wlr_color_manager_v1` directly, so
   without it the compositor does not compile; `xwayland` needs the `xcb-*`
   development packages present when wlroots is configured, and comes out `NO`
   silently if they are not. Check the feature summary meson prints.

2. **Build asteroidz**
   There is no separate scenefx step. The effects library —
   `asteroidz-scenefx`, asteroidz's own fork, not upstream `wlrfx/scenefx` —
   lives in this repository at `subprojects/asteroidz-scenefx` and is built
   and linked **statically** as part of the compositor. Nothing to install
   alongside, no `libasteroidz-scenefx-0.5.so` to keep in version step.

   ```bash
   git clone https://github.com/asteroidzman/asteroidz.git
   cd asteroidz
   meson build -Dprefix=/usr
   sudo ninja -C build install
   ```

   `-Dprefix=/usr` is not optional in practice: the bar's icon search path is
   Build options: `-Dxwayland=disabled` drops X11 support, and `-Dtracy=true` builds
   with [Tracy](https://github.com/wolfpld/tracy) profiler instrumentation —
   off by default, and it fetches the Tracy client over the network, so it is
   not suitable for an offline or clean-chroot build.

   This installs the `asteroidz` binary, the `amsg` IPC tool, three wayland
   session entries, and the GlobalShortcuts portal definition.

   | session | what composites the frame |
   |---|---|
   | **Asteroidz** | SceneFX on wlroots' GLES2 renderer — the daily driver and default |
   | **Asteroidz (Vulkan, experimental)** | SceneFX on wlroots' Vulkan renderer (`fx_vk`) |
   | **Asteroidz (AVK native Vulkan)** | asteroidz's own Vulkan engine, no `wlr_renderer` involved |

   The AVK session is under construction — no effects, no colour management,
   full damage every frame — and it pins `WLR_RENDERER=gles2` on purpose:
   wlroots still needs a renderer for shm formats, the allocator and
   screencopy, none of which are composition. If AVK cannot render an output
   correctly (a colour-managed display, a software cursor) it hands that frame
   back to SceneFX and says so in the log rather than drawing it wrongly.

   There is a third, separate switch: `ASTEROIDZ_RENDERER=avk` selects
   asteroidz's own native Vulkan engine, which composites the desktop itself
   instead of going through a `wlr_renderer` at all. It is under construction
   — no effects, no colour management, no partial damage yet — and it is
   deliberately independent of `WLR_RENDERER`, so
   `WLR_RENDERER=gles2 ASTEROIDZ_RENDERER=avk` is a valid and useful pairing.
   See [`docs/vulkan-native-architecture.md`](./vulkan-native-architecture.md).

   asteroidz uses the GLES2 renderer by default; the renderer is selected
   per session via `WLR_RENDERER` (`gles2` or `vulkan`). Vulkan is
   experimental — near feature parity for everyday use, pending future
   wlroots enhancements before it's the recommended default. Some
   native-Wayland GPU apps (e.g. Electron) don't yet import on the Vulkan
   renderer — run them under XWayland, or just use the default GLES2
   session.
