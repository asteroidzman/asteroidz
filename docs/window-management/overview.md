---
title: Overview
description: Configure the overview mode for window navigation.
---

## Overview Settings

| Setting | Type | Default | Description |
| :--- | :--- | :--- | :--- |
| `hotarea_size` | integer | `10` | Hot area size in pixels. |
| `enable_hotarea` | integer | `0` | Enable hot areas (0: disable, 1: enable). |
| `hotarea_corner` | integer | `2` | Hot area corner (0: top-left, 1: top-right, 2: bottom-left, 3: bottom-right). |
| `ov_tab_mode` | integer | `1` | Overview tab mode (0: disable, 1: enable). |
| `overviewgappi` | integer | `5` | Inner gap in overview mode. |
| `overviewgappo` | integer | `30` | Outer gap in overview mode. |
| `ov_no_resize` | integer | `1` | Disable window resizing in overview mode. |

### Setting Descriptions

- `enable_hotarea` — Toggles overview when the cursor enters the configured corner.
- `hotarea_size` — Size of the hot area trigger zone in pixels.
- `hotarea_corner` — Corner that triggers the hot area (0: top-left, 1: top-right, 2: bottom-left, 3: bottom-right).
- `ov_tab_mode` — Circles focus through windows in overview; releasing the mod key exits overview.
- `ov_no_resize` — Disables resizing of windows in overview mode(use snap to display). When enabled (the default), the overview is a macOS-Exposé-style spread: every window is scaled down at its real aspect ratio and bin-packed so nothing overlaps.

The name of the tag you came from is shown as a label centered at the top of the overview. Give tags names with the `name:` [tag rule](./rules.md#tag-rules) or the `set_tag_name` dispatcher (e.g. `Super+N { set_tag_name web; }` in a `binds` block); unnamed tags show their number.

### Mouse Interaction in Overview

When in overview mode:

- **Left mouse button** — Jump to (focus) a window.
- **Right mouse button** — Close a window.