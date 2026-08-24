# Known issues

Open defects with what has been established and, as importantly, what has been
*ruled out*. The point of this file is that the next attempt starts where the
last one stopped instead of re-deriving it.

## OPEN — M14B: inter prediction decodes to noise on real content

Found 2026-08-21, by recording a real desktop for the first time. The file was
green. `ffmpeg`: *"The cu_qp_delta 113 is outside the valid range [-32, 31]"*,
which is a decoder that has desynced and is reading residual bits as syntax.

**Bisected.** A detailed single IDR decodes clean. A detailed sequence with P
pictures does not. Flat content decodes clean either way. So it is inter
prediction on content with real residual, and nothing else.

**Ruled out, each by building it and re-measuring:** `cu_qp_delta_enabled_flag`,
`sample_adaptive_offset_enabled_flag` with its slice flags, transform hierarchy
depth 0 and 1, `amp_enabled_flag`, and the reference picture's `pic_type` (which
described the previous picture as P when the first one is an IDR -- a real bug,
fixed, and not this one).

**The finding that reframes it.** The driver writes its OWN PPS and ignores the
flags in `StdVideoH265PictureParameterSet`. Parsed out of the emitted stream:
`cu_qp_delta_enabled_flag` is 1 and `dependent_slice_segments_enabled_flag` is
1, and neither was set here. So every syntax flag bisected above was
decorative, and whatever disagrees does so between the driver's own parameter
sets and the slice header or picture info this code supplies.

**What ships meanwhile: all-intra.** Every frame is a key frame. The files are
several times larger and every one of them is right, which is the correct way
round for a recording somebody keeps -- and all-intra is what capture tools
choose deliberately, because it seeks and cuts anywhere. `AZ_ENCODE_INTER=1`
reaches the broken path for whoever picks this up.

**The lesson, which cost more than the bug.** Everything that exercised the
encoder encoded a FLAT COLOUR. A flat picture has almost no residual, so a
syntax desync has almost nothing to corrupt: every check passed, ffprobe agreed
on every header, and the first real desktop was green. Anything written against
this path has to move high-frequency detail -- luma std 215 against a flat
field's 0.6 -- or it is measuring nothing.

## OPEN — teardown aborts in glibc after overview/jump plus a screenshot

Found 2026-08-20. **Reproducer established 2026-08-24**, which the entry
previously lacked.

**Reproduce.** Headless, one output, effects on (blur, shadows, border radius),
three mapped clients, under `ASTEROIDZ_VK_DEBUG=1` and
`VK_LOADER_LAYERS_ENABLE='*validation*'`. Dispatch, with ~1.5s between each:

    toggle_overview,jump
    toggle_overview
    set_layout,tile
    toggle_overview
    toggle_overview
    screenshot_ui,screen
    kill_client  x3

then SIGTERM. Exit 134 or 139. The log ends:

```
avk: validation: UNASSIGNED-Threading-Info: vkDestroyBuffer():
     Couldn't find VkBuffer Object 0xc600000000c6.   (x2)
corrupted size vs. prev_size
```

**`screenshot_ui` is required.** Overview and jump alone do not reproduce it --
verified across single and dual output, effects on and off, with and without
`capture_output`, all clean with 0 VUIDs. The original entry named
overview/jump because that is what the fixture's name suggested; the screenshot
step was the ingredient nobody had subtracted.

**The Vulkan message is probably collateral, not the cause.** Tracing every
`avk_upload_finish` through a reproducing run recorded 20 frees and NO
duplicate handle, so nothing freed that buffer twice through the upload path.
`UNASSIGNED-Threading-Info` is the validation layer failing to find an object in
its own map, and `corrupted size vs. prev_size` is glibc rejecting a mangled
chunk header -- both are what a HOST heap corruption looks like from two
different observers. The next step is ASAN or valgrind on the sequence above,
not further reading.

**Ruled out by inspection, so nobody re-derives them.**

- `az_avk_surface_adopt_image()` clears `entry->image`, so the surface and the
  retire queue never both own it.
- The gradient slot grow path clears `slot->buffer/memory/mapped` before
  pushing the old pair to the retire queue.
- `avk_oracle` nulls both handles in `az_oracle_drop_buffer()`, and `ref_image`
  is nulled at every destroy.
- `staging_cache_put`/`staging_cache_take` are symmetric; teardown fills the
  cache (`avk_dmabuf_importer_finish`) before draining it
  (`avk_device_destroy`), which is the right order.

**One latent inconsistency found on the way, not the cause.**
`az_avk_buffer_destroy()` copies `entry->upload[s]` to the retire queue without
the `memset(up, 0, sizeof(*up))` that both other retire sites perform. It is
harmless today only because `free(entry)` follows with nothing reading the slots
in between -- which is a property of the code around it rather than of the copy.

## OPEN — a tag switch arranges twice

A tag switch emits two `anim start`s per client one frame apart (83 of 250 in a
20-round-trip run were superseded before their first tick). Harmless now that a
replaced-but-unsampled segment restarts cleanly, but the second arrange is
redundant work.
