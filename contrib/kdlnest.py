#!/usr/bin/env python3
"""Convert a legacy asteroidz `key=value` config into nested (Niri-style) KDL.

Usage: kdlnest.py <in.conf> [out.kdl]

Emits sections (input/decoration/layout/overview/animations/misc), keychord
`binds`/`mouse-binds`, `window-rule` match/action, `tag`/`output` blocks,
`environment`, and `spawn-at-startup`. Mirrors the compositor's kdl_key_map
(src/config/parse_config.h); options with no nice mapping are emitted under
`misc` by their canonical key (the parser falls back to leaf-name=key).
"""
import re
import sys
from collections import OrderedDict

# canonical option key -> nested path "section/sub/leaf" (inverse of the C map)
KEY_MAP = {
    "xkb_layout": "input/keyboard/xkb/layout",
    "repeat_delay": "input/keyboard/repeat/delay",
    "repeat_rate": "input/keyboard/repeat/rate",
    "cursor_theme": "input/cursor/theme",
    "cursor_size": "input/cursor/size",
    "enable_titlebar": "layout/titlebar/enable",
    "titlebar_height": "layout/titlebar/height",
    "borderpx": "layout/border/width",
    "bordercolor": "layout/border/color",
    "focuscolor": "layout/border/focus-color",
    "urgentcolor": "layout/border/urgent-color",
    "border_gradient": "layout/border/gradient/enable",
    "border_gradient_angle": "layout/border/gradient/angle",
    "border_gradient_color2": "layout/border/gradient/color2",
    "monocle_tab_max_width": "layout/monocle/tab-max-width",
    "scroller_structs": "layout/scroller/structs",
    "scroller_default_proportion": "layout/scroller/default-proportion",
    "scroller_default_proportion_single": "layout/scroller/default-proportion-single",
    "scroller_focus_center": "layout/scroller/focus-center",
    "scroller_prefer_center": "layout/scroller/prefer-center",
    "scroller_proportion_preset": "layout/scroller/preset",
    "scroller_edge_scroll": "layout/scroller/edge-scroll/enable",
    "scroller_edge_scroll_size": "layout/scroller/edge-scroll/size",
    "scroller_edge_scroll_delay": "layout/scroller/edge-scroll/delay",
    "edge_scroller_pointer_focus": "layout/scroller/edge-scroll/pointer-focus",
    "edge_scroller_focus_allow_speed": "layout/scroller/edge-scroll/allow-speed",
    "blur": "effects/blur/enable",
    "blur_layer": "effects/blur/layer",
    "blur_optimized": "effects/blur/optimized",
    "blur_unfocused_strength": "effects/blur/unfocused-strength",
    "blur_params_num_passes": "effects/blur/passes",
    "blur_params_radius": "effects/blur/radius",
    "blur_params_noise": "effects/blur/params/noise",
    "blur_params_brightness": "effects/blur/params/brightness",
    "blur_params_contrast": "effects/blur/params/contrast",
    "blur_params_saturation": "effects/blur/params/saturation",
    "blur_transparency_threshold": "effects/blur/transparency-threshold",
    "shadows": "effects/shadow/enable",
    "layer_shadows": "effects/shadow/layer",
    "shadow_only_floating": "effects/shadow/only-floating",
    "shadows_size": "effects/shadow/size",
    "shadows_blur": "effects/shadow/blur",
    "shadows_position_x": "effects/shadow/position/x",
    "shadows_position_y": "effects/shadow/position/y",
    "shadowscolor": "effects/shadow/color",
    "shadows_unfocused_scale": "effects/shadow/unfocused-scale",
    "shadows_tiled_scale": "effects/shadow/tiled-scale",
    "shadows_contact": "effects/shadow/contact/enable",
    "shadows_contact_size": "effects/shadow/contact/size",
    "shadows_contact_blur": "effects/shadow/contact/blur",
    "shadows_contact_position_x": "effects/shadow/contact/position/x",
    "shadows_contact_position_y": "effects/shadow/contact/position/y",
    "shadowscolor_contact": "effects/shadow/contact/color",
    "pill_decorate_border_width": "pill/border-width",
    "pill_decorate_corner_radius": "pill/corner-radius",
    "pill_decorate_padding_x": "pill/padding/x",
    "pill_decorate_padding_y": "pill/padding/y",
    "pill_decorate_font_desc": "pill/font",
    "pill_decorate_bg_color": "pill/bg-color",
    "pill_decorate_fg_color": "pill/fg-color",
    "pill_decorate_focus_bg_color": "pill/focus-bg-color",
    "pill_decorate_focus_fg_color": "pill/focus-fg-color",
    "animation_curve_type": "animations/curve",
    "spring_damping": "animations/spring/damping",
    "spring_frequency": "animations/spring/frequency",
    "animation_type_open": "animations/window-open/type",
    "animation_duration_open": "animations/window-open/duration",
    "fadein_begin_opacity": "animations/window-open/fade-begin-opacity",
    "animation_type_close": "animations/window-close/type",
    "animation_duration_close": "animations/window-close/duration",
    "fadeout_begin_opacity": "animations/window-close/fade-begin-opacity",
    "overviewgappi": "overview/gaps/inner",
    "overviewgappo": "overview/gaps/outer",
    "hotarea_size": "overview/hotarea/size",
    "enable_hotarea": "overview/hotarea/enable",
    "ov_tab_mode": "overview/tab-mode",
    "ov_no_resize": "overview/no-resize",
    "xwayland_persistence": "misc/xwayland-persistence",
    "syncobj_enable": "misc/syncobj",
    "focus_on_activate": "misc/focus-on-activate",
    "allow_tearing": "misc/allow-tearing",
    "sdr_reference_luminance": "misc/sdr/reference-luminance",
    "sdr_saturation": "misc/sdr/saturation",
    "dpms_wake_retrain": "misc/dpms-wake-retrain",
    "drag_tile_to_tile": "misc/drag-tile-to-tile",
    "icon_theme": "misc/icon-theme",
}
RULE_MAP = {  # windowrule field -> nice
    "appid": ("match", "app-id"), "title": ("match", "title"),
    "isfloating": ("act", "open-floating"), "isfullscreen": ("act", "open-fullscreen"),
    "noblur": ("act", "no-blur"), "isnoborder": ("act", "no-border"),
    "isnoshadow": ("act", "no-shadow"), "isnoradius": ("act", "no-rounding"),
    "isnoanimation": ("act", "no-animation"),
}
OUTPUT_MAP = {"bitdepth": "bit-depth", "icc_profile": "icc-profile",
              "hdr_max_luminance": "max-luminance", "hdr_min_luminance": "min-luminance",
              "hdr_max_fall": "max-fall"}
MOD_BACK = {"SUPER": "Super", "CTRL": "Ctrl", "ALT": "Alt", "SHIFT": "Shift"}
BIND_RE = re.compile(r"^bind[slrp]*$")
NUM_RE = re.compile(r"^-?\d+(\.\d+)?$")


def q(s):
    if s == "":
        return '""'
    if NUM_RE.match(s):
        return s
    if s in ("true", "false", "null"):
        return '"%s"' % s
    if re.match(r'^[^\s{}()="\\;/]+$', s):
        return s
    return '"%s"' % s.replace("\\", "\\\\").replace('"', '\\"')


class Tree:
    """nested section accumulator"""
    def __init__(self):
        self.leaves = OrderedDict()   # name -> value(str) at this level
        self.subs = OrderedDict()     # name -> Tree

    def put(self, path, value):
        parts = path.split("/")
        node = self
        for p in parts[:-1]:
            node = node.subs.setdefault(p, Tree())
        node.leaves[parts[-1]] = value

    def render(self, indent=0):
        pad = "    " * indent
        out = []
        for k, v in self.leaves.items():
            out.append(f"{pad}{k} {q(v)}" if v != "" else f"{pad}{k}")
        for name, sub in self.subs.items():
            out.append(f"{pad}{name} {{")
            out.append(sub.render(indent + 1))
            out.append(f"{pad}}}")
        return "\n".join(out)


def convert(lines):
    root = Tree()
    binds, mbinds, rules, tags, outputs, env, spawn, source = (
        [], [], [], [], [], [], [], [])
    for raw in lines:
        line = raw.rstrip("\n")
        s = line.strip()
        if s == "" or s.startswith("#"):
            continue
        if "=" not in line:
            continue
        key, val = line.split("=", 1)
        key, val = key.strip(), val.strip()

        if BIND_RE.match(key) or key == "mousebind":
            f = val.split(",")
            mods = "+".join(MOD_BACK.get(m, m) for m in f[0].split("+") if m)
            chord = (mods + "+" + f[1]) if mods else f[1]
            act = f[2] if len(f) > 2 else ""
            args = " ".join(q(a) for a in f[3:])
            block = f"{chord} {{ {act}{(' ' + args) if args else ''}; }}"
            (mbinds if key == "mousebind" else binds).append(block)
        elif key == "env":
            f = val.split(",", 1)
            env.append(f'{f[0]} {q(f[1] if len(f) > 1 else "")}')
        elif key in ("exec-once", "exec"):
            spawn.append(("spawn-at-startup" if key == "exec-once" else "spawn", val))
        elif key in ("source", "source-optional"):
            source.append((key, val[:-5] + ".kdl" if val.endswith(".conf") else val))
        elif key == "windowrule":
            match, act = [], []
            for fld in val.split(","):
                if ":" not in fld:
                    continue
                fk, fv = fld.split(":", 1)
                where, nice = RULE_MAP.get(fk, ("act", fk))
                if where == "match":
                    match.append(f'{nice}={q(fv)}')
                elif fv == "1" and nice.startswith(("open-", "no-")):
                    act.append(nice)
                else:
                    act.append(f"{nice} {q(fv)}")
            rules.append((match, act))
        elif key == "tagrule":
            fid, ch = "", []
            for fld in val.split(","):
                if ":" not in fld:
                    continue
                fk, fv = fld.split(":", 1)
                if fk == "id":
                    fid = fv
                elif fk == "layout_name":
                    ch.append(f"layout {q(fv)}")
                else:
                    ch.append(f"{fk} {q(fv)}")
            tags.append((fid, ch))
        elif key == "monitorrule":
            name, ch = "", []
            for fld in val.split(","):
                if ":" not in fld:
                    continue
                fk, fv = fld.split(":", 1)
                if fk == "name":
                    name = fv.strip("^$")
                else:
                    nice = OUTPUT_MAP.get(fk, fk)
                    ch.append(nice if fv == "1" else f"{nice} {q(fv)}")
            outputs.append((name, ch))
        elif key in KEY_MAP:
            root.put(KEY_MAP[key], val)
        else:
            root.put("misc/" + key, val)  # fallback: canonical name under misc

    out = []
    body = root.render()
    if body:
        out.append(body)
    if env:
        out.append("environment {\n" + "\n".join("    " + e for e in env) + "\n}")
    for kind, cmd in spawn:
        out.append(f"{kind} " + " ".join(q(t) for t in cmd.split(" ")))
    if binds:
        out.append("binds {\n" + "\n".join("    " + b for b in binds) + "\n}")
    if mbinds:
        out.append("mouse-binds {\n" + "\n".join("    " + b for b in mbinds) + "\n}")
    for fid, ch in tags:
        inner = "; ".join(ch)
        out.append(f'tag {q(fid)} {{ {inner} }}' if inner else f'tag {q(fid)} {{}}')
    for match, act in rules:
        parts = []
        if match:
            parts.append("match " + " ".join(match))
        parts.extend(act)
        out.append("window-rule { " + "; ".join(parts) + " }")
    for name, ch in outputs:
        inner = "; ".join(ch)
        out.append(f'output {q(name)} {{ {inner} }}' if inner else f'output {q(name)} {{}}')
    for kind, path in source:
        out.append(f'{kind} {q(path)}')
    return "\n\n".join(out) + "\n"


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    with open(sys.argv[1]) as f:
        text = convert(f.readlines())
    if len(sys.argv) >= 3:
        open(sys.argv[2], "w").write(text)
    else:
        sys.stdout.write(text)


if __name__ == "__main__":
    main()
