---
title: Window Effects
description: Add visual polish with blur, shadows, and opacity.
---

## Blur

Blur creates a frosted glass effect for transparent windows.

| Setting | Default | Description |
| :--- | :--- | :--- |
| `blur` | `0` | Enable blur for windows. |
| `blur_layer` | `0` | Enable blur for layer surfaces (like bars/docks). |
| `blur_optimized` | `1` | Caches the wallpaper and blur background, significantly reducing GPU usage. Disabling it will significantly increase GPU consumption and may cause rendering lag. **Highly recommended.** |
| `blur_params_radius` | `5` | The strength (radius) of the blur. |
| `blur_params_num_passes` | `2` | Number of passes. Higher = smoother but more expensive. `1` leaves visible bleed-through of background detail; `2` is the minimum that reads as a proper blur. |
| `blur_params_noise` | `0.02` | Blur noise level. |
| `blur_params_brightness` | `0.9` | Blur brightness adjustment. |
| `blur_params_contrast` | `0.9` | Blur contrast adjustment. |
| `blur_params_saturation` | `1.2` | Blur saturation adjustment. |

> **Warning:** Blur has a relatively high impact on performance. If your hardware is limited, it is not recommended to enable it. If you experience lag with blur on, ensure `blur_optimized=1` — disabling it will significantly increase GPU consumption and may cause rendering lag. To disable blur entirely, set `blur=0`.

---

## Shadows

Drop shadows help distinguish floating windows from the background.

| Setting | Default | Description |
| :--- | :--- | :--- |
| `shadows` | `0` | Enable shadows. |
| `layer_shadows` | `0` | Enable shadows for layer surfaces. |
| `shadow_only_floating` | `1` | Only draw shadows for floating windows (saves performance). |
| `shadows_size` | `10` | Size of the shadow. |
| `shadows_blur` | `15` | Shadow blur amount. |
| `shadows_position_x` | `0` | Shadow X offset. |
| `shadows_position_y` | `0` | Shadow Y offset. |
| `shadows_contact` | `1` | Enable the macOS-style tight "contact" shadow layered above the ambient one |
| `shadows_contact_size` | `8` | Contact shadow spread in px |
| `shadows_contact_blur` | `9` | Contact shadow blur sigma |
| `shadows_contact_position_x` | `0` | Contact shadow X offset |
| `shadows_contact_position_y` | `2` | Contact shadow Y offset |
| `shadowscolor_contact` | `0x0000004d` | Contact shadow color (RGBA hex) |
| `shadows_unfocused_scale` | `0.45` | Shadow opacity multiplier for unfocused windows (macOS-style dimming) |
| `shadowscolor` | `0x000000ff` | Color of the shadow. |

```kdl
effects {
    shadow {
        enable 1
        layer 1
        only-floating 1
        size 12
        blur 15
        color 0x000000ff
        position {
            x 0
            y 0
        }
    }
}
```

---

## Opacity & Corner Radius

Control the transparency and roundness of your windows.

| Setting | Default | Description |
| :--- | :--- | :--- |
| `border_radius` | `0` | Window corner radius in pixels. |
| `border_radius_location_default` | `15` | Corner-location BITMASK for the radius: `1` top-left, `2` top-right, `4` bottom-right, `8` bottom-left; combine by adding (`15` = all, `0` = none). |
| `no_radius_when_single` | `0` | Disable radius if only one window is visible. |
| `focused_opacity` | `1.0` | Opacity for the active window (0.0 - 1.0). |
| `unfocused_opacity` | `1.0` | Opacity for inactive windows (0.0 - 1.0). |

```kdl
misc {
    border_radius 0
    border_radius_location_default 15
    no_radius_when_single 0
    focused_opacity 1.0
    unfocused_opacity 1.0
}
```
