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
     defaultSession = "asteroidz-avk"; # derived from asteroidz-avk.desktop filename
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

You will need to build `wlroots` manually as well.

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

   **0.20.2, not 0.20.0.** The scene graph reads
   `wlr_surface_output.suspended`, which arrived after the `.0` release — against
   0.20.0 the build gets a hundred files in and then fails on a struct member.
   `meson.build` requires `>=0.20.2` so this is caught at configure time now.

   **Both flags matter and neither is on by default.** `color-management`
   requires `lcms2` and asteroidz uses `wlr_color_manager_v1` directly, so
   without it the compositor does not compile; `xwayland` needs the `xcb-*`
   development packages present when wlroots is configured, and comes out `NO`
   silently if they are not. Check the feature summary meson prints.

2. **Build asteroidz**
   There is no scenefx step at all any more, and no subproject. The scene
   graph that used to come from `asteroidz-scenefx` is asteroidz source now,
   in `src/scene/` — so there is nothing to install alongside, no
   `libasteroidz-scenefx-0.5.so`, and no ABI marker to keep in version step.

   ```bash
   git clone https://github.com/asteroidzman/asteroidz.git
   cd asteroidz
   meson build -Dprefix=/usr
   sudo ninja -C build install
   ```

   `-Dprefix=/usr` is not optional in practice: the bar's icon search path is
   Build options: `-Dxwayland=disabled` drops X11 support.

   There is no profiler build option. Tracy instrumentation was removed in
   0.25.1: its client came from the SceneFX subproject, so it stopped building
   the moment that subproject was deleted, and what it measured is covered by
   `amsg get avk-stats` (GPU and CPU percentiles, timing histograms that name
   outlier frames), the presenter's error series, and `AZ_PACE`. The build no
   longer fetches anything over the network.

   This installs the `asteroidz` binary, the `amsg` IPC tool, two wayland
   session entries, and the GlobalShortcuts portal definition.

   | session | what it is |
   |---|---|
   | **Asteroidz (AVK native Vulkan)** | the desktop. asteroidz's own Vulkan engine composites every frame |
   | **Asteroidz (AVK + Vulkan validation)** | the same, with the Vulkan validation layers on — for acceptance runs only |

   Pick the first one. The second exists so an acceptance run can prove its
   own environment rather than claiming it, and it costs roughly 99x the CPU
   per frame; it is not a session to work in.

   Both pin `WLR_RENDERER=vulkan` on purpose, and that is not a contradiction:
   wlroots still needs a renderer for shm formats, the allocator and
   screencopy, none of which are composition. Who draws the desktop is not a
   choice at all — AVK does, always — and `WLR_RENDERER` has no bearing on it.
   See [`docs/architecture.md`](./architecture.md).
