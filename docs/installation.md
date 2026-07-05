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
         url = "github:ralfwierzbicki/asteroidz";
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
       inputs.asteroidz.nixosModules.mango
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
           asteroidz.nixosModules.mango # or inputs.asteroidz.nixosModules.mango
           # other imports ...
         ];
       };
     }
   }
   ```

   > **Note:** the module/option is still named `mango`/`programs.mango` internally
   > (not yet renamed to match the compositor's `asteroidz` identity) — that's a
   > separate, breaking change for a future pass. The flake input alias above
   > (`asteroidz`) is just your own name for it and can be anything.

3. **Enable the module**

   ```nix
   # configuration.nix (or any other file that you import)
   {
     programs.mango.enable = true;
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

You will need to build `wlroots` and asteroidz's `scenefx` fork manually as well.

1. **Build wlroots**
   asteroidz currently tracks wlroots 0.20 (check `meson.build` for the exact
   required version).

   ```bash
   git clone -b 0.20.0 https://gitlab.freedesktop.org/wlroots/wlroots.git
   cd wlroots
   meson build -Dprefix=/usr
   sudo ninja -C build install
   ```

2. **Build asteroidz-scenefx**
   This library handles the visual effects. asteroidz requires its own fork,
   not upstream `wlrfx/scenefx` — the two are not interchangeable.

   ```bash
   git clone https://github.com/ralfwierzbicki/asteroidz-scenefx.git
   cd asteroidz-scenefx
   meson build -Dprefix=/usr
   sudo ninja -C build install
   ```

3. **Build asteroidz**
   Finally, compile the compositor itself.
   ```bash
   git clone https://github.com/ralfwierzbicki/asteroidz.git
   cd asteroidz
   meson build -Dprefix=/usr
   sudo ninja -C build install
   ```

   This installs the `asteroidz` binary, the `amsg` IPC tool, a wayland
   session entry, and the GlobalShortcuts portal definition.
