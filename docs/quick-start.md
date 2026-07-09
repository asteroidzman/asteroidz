---
title: Quick Start
description: Basic configuration and first steps with asteroidz.
---

Now that you have asteroidz installed, let's get your environment set up.

## Initial Setup

1. **Create Configuration Directory**

   asteroidz looks for configuration files in `~/.config/asteroidz/`.

   ```bash
   mkdir -p ~/.config/asteroidz
   ```

2. **Copy Default Config**

   A default configuration file is provided at `/etc/asteroidz/config.kdl`. Copy it to your local directory to start customizing.

   ```bash
   cp /etc/asteroidz/config.kdl ~/.config/asteroidz/config.kdl
   ```

3. **Launch asteroidz**

   You can now start the compositor from your TTY.

   ```bash
   asteroidz
   ```

   Optional: To specify a custom config file path:

   ```bash
   asteroidz -c /path/to/your/config.kdl
   ```

## Essential Keybindings

asteroidz uses the following keybinds by default:

| Key Combination | Action |
| :--- | :--- |
| `Alt` + `Return` | Open Terminal (defaults to `foot`) |
| `Alt` + `Space` | Open Launcher (defaults to `rofi`) |
| `Alt` + `Q` | Close (Kill) the active window |
| `Super` + `M` | Quit asteroidz |
| `Super` + `F` | Toggle Fullscreen |
| `Alt` + `Arrow Keys` | Move focus (Left, Right, Up, Down) |
| `Ctrl` + `1-9` | Switch to Tag 1-9 |
| `Alt` + `1-9` | Move window to Tag 1-9 |

> **Warning:** Some default bindings rely on specific tools like `foot` (terminal) and `rofi` (launcher). Ensure you have them installed or update your `config.kdl` to use your preferred alternatives.

## Recommended Tools

To get a fully functional desktop experience, we recommend installing the following components:

| Category | Recommended Tools |
| :--- | :--- |
| Application Launcher | rofi, bemenu, wmenu, fuzzel |
| Terminal Emulator | foot, wezterm, alacritty, kitty, ghostty |
| Status Bar | waybar, eww, quickshell, ags |
| Desktop Shell | Noctalia, DankMaterialShell |
| Wallpaper Setup | awww(swww), swaybg |
| Notification Daemon | swaync, dunst, mako |
| Desktop Portal | xdg-desktop-portal, xdg-desktop-portal-wlr, xdg-desktop-portal-gtk |
| Clipboard | wl-clipboard, wl-clip-persist, cliphist |
| Gamma Control / Night Light | wlsunset, gammastep |
| Miscellaneous | xfce-polkit, wlogout |

## Example Configuration

Check out the [example configuration](https://github.com/DreamMaoMao/mango-config) by the creator of upstream mango, including complete setups for mango, Waybar, Rofi, and more.

> **Note:** This is an upstream mango config, not asteroidz-specific. Several
> options referenced there (`tab_bar_decorate_*`, `group_bar_decorate_*`,
> `jump_label_decorate_*`, `~/.config/mango`) have since been renamed or
> merged in this fork — see [Theming](/docs/visuals/theming) — so treat it as
> a starting point to adapt, not a drop-in config.

```bash
git clone https://github.com/DreamMaoMao/mango-config.git ~/.config/asteroidz
```

## Next Steps

Now that you are up and running, dive deeper into customizing asteroidz:

- [Configure Monitors](/docs/configuration/monitors) — Set up resolution, scaling, and multi-monitor layouts.
- [Window Rules](/docs/window-management/rules#window-rules) — Define how specific applications should behave.
- [Appearance](/docs/visuals/theming) — Customize colors, borders, gaps, and effects.
