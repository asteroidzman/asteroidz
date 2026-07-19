# Headless animation/effects test harness

`contrib/anim-test.sh` lets you **see** rendering changes (window open/close
animations, blur, borders, effects) without a real display — essential for
catching frame-accurate bugs like a one-frame flash on window open/close.
For assertion-based window-management/IPC dispatch regressions instead of
visual ones, see [the regression test harness](./regression-testing.md).

It launches a **second asteroidz instance on a virtual (headless) output** in a
dedicated, throwaway `XDG_RUNTIME_DIR`, so it never touches your real session.
It sets a patterned wallpaper, opens a translucent terminal, then records an
open + close of a second window and extracts the frames for inspection.

## Usage

```sh
contrib/anim-test.sh [CONFIG] [LABEL]
```

- `CONFIG` — KDL config to run. Omit (or pass `""`) to use a generated minimal
  config with blur + spring open/close fade. Keep test configs free of
  `spawn-at-startup` / DMS so they can't disturb your session.
- `LABEL` — output basename (default `anim`).

Useful env: `ASTEROIDZ` (binary under test, default `/usr/bin/asteroidz`),
`WALLPAPER` (backdrop image; a patterned one makes sharp-vs-blurred obvious),
`OPEN_KDL` (override the `window-open` line), `FPS`, `OUTDIR`.

## Output (under `$OUTDIR`, default `/tmp/asteroidz-anim`)

- `rec.mp4` — the recording
- `fr/f_*.png` — extracted frames
- `<LABEL>.png` — a montage scan of the whole clip
- printed list of the **brightest frames** — a full-window colour "flash"
  (e.g. a border/focus-colour fill) shows up as a bright outlier vs the
  ~steady state, which is how the open/close flash was found and fixed.

## Requirements

`wf-recorder`, `ffmpeg`, ImageMagick (`montage`/`convert`), `mpvpaper`,
`kitty`, and a wlroots headless-capable asteroidz build.

## Workflow for a rendering fix

1. Reproduce: run the harness, inspect the montage / brightest frames.
2. Change code, rebuild + install (or point `ASTEROIDZ` at a build).
3. Re-run the harness; compare montages / brightest-frame values.
4. Only ship once the harness confirms the change visually.
