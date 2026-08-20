# Known issues

Open defects with what has been established and, as importantly, what has been
*ruled out*. The point of this file is that the next attempt starts where the
last one stopped instead of re-deriving it.

## M14 — what a capture client is actually offered

Measured 2026-08-20 with `contrib/wlcapture`, the ext-image-copy-capture client
M14's audit found was missing. Recorded here because the numbers change what M14
has to build first.

**The offer, headless 1920x1080:**

```
shm formats 1
  shm       XRGB8888       8 bpc
dmabuf formats 2
  dmabuf    XRGB8888       8 bpc
  dmabuf    ARGB8888       8 bpc
shm depth   8 bpc max
```

Nothing above 8 bits per channel, in either family. "Portal capture forces the
output down to SDR" is therefore not a portal defect and not a pipewire one: it
is what this compositor advertises to any capture client.

**Throughput is not the problem.** With a client repainting every frame,
`wlcapture` took 30 frames in 1.64s — **17.7 fps**, against `hdr-record.sh`'s
1 fps. ext-image-copy-capture is already a working stream. What the 1 fps
measures is the `screenshot_ui` freeze-and-read-back path, exactly as the audit
said, and not a ceiling on capture itself. (17.7 fps is a floor, not a maximum:
the worst inter-frame gap was 101ms and the repainting client is the likely
pacer, not the capture path.)

**The offer is wlroots', not ours.** `asteroidz.c:11598` calls
`wlr_ext_output_image_capture_source_manager_v1_create()`, and
`wlr_ext_image_capture_source_v1` carries `shm_formats` and `dmabuf_formats` as
its own fields. asteroidz's only involvement in a capture session today is
`handle_image_copy_capture_new_session()`, which counts sessions for the privacy
shield and nothing else. So D6's "sessions backed by AVK stage taps" is not an
enhancement of the current path — it means asteroidz implementing its own
capture source instead of registering wlroots'.

**Not yet measured: the HDR output.** The headless backend refuses HDR (it
reverts `m->hdr` on the next commit, see `contrib/regression/tests/hdr.sh`), so
whether a real HDR output changes the offer needs a live run.

## OPEN — teardown frees a VkDeviceMemory twice after overview/jump

Found 2026-08-20 by `contrib/render-matrix-test.sh`, which had never been run:
it sat outside `avk-suite.sh`'s discovery glob until the register was widened to
every `.sh`.

**Symptom.** After a session that opens the overview and jump mode, exit reaches
`vkFreeMemory()` with a handle Vulkan does not recognise, then segfaults inside
`vkDestroyDevice`. `AVK_TEARDOWN_END` and `CLEANUP_END` never print.

```
avk: validation: VUID-vkFreeMemory-memory-parameter: vkFreeMemory():
     memory Invalid VkDeviceMemory Object 0x7740000000774
avk: validation: UNASSIGNED-Threading-Info: vkFreeMemory():
     Couldn't find VkDeviceMemory Object 0x7740000000774   (x2)

#7  avk_device_destroy (dev=…)   ../src/render/vulkan/device/avk_device.c:425
#8  az_avk_finish ()             ../src/render/az_avk.h:9348
#9  cleanup ()                   ../src/asteroidz.c:4713
#10 main ()                      ../src/asteroidz.c:13631
```

**Reproduce.** `bash contrib/render-matrix-test.sh` — 2 of 2 arms (SDR and HDR),
exit 139 on both. Note it truncates `~/.local/state/asteroidz/asteroidz.log` to
get a clean VUID count.

**Established.**

- The double free is real. The crash is *inside the validation layer*, which
  corrupts its object map on the bad free and then walks it at device destroy —
  so a session without `ASTEROIDZ_VK_DEBUG` does not visibly crash, but is
  executing the same undefined behaviour.
- AVK's own leak ledger reports `device_memory=0 … (all zero)` immediately
  after. Its accounting does not see the second free, which is why nothing else
  caught it.
- `contrib/avk-teardown-test.sh` is `required` and passes. It never drives
  overview or jump — zero matches for either — so the required set structurally
  could not reach this path.

**Not yet established.** Whether the trigger is overview, jump, or the tag/layout
sequence around them; whether it predates 0.26.0; whether it reproduces without
the validation layer.

## FIXED — tag-in animation sometimes did not run

Reported 2026-08-17, fixed the same day. Kept because four hypotheses were
falsified before the real one, and because the instrument that finally answered
it is the reusable part.

**Symptom.** Switching tags sometimes animated and sometimes did not; when it
half-ran it "fell short". Intermittent, no known trigger.

**Cause.** A tag switch arranges twice in the same event-loop pass, so a segment
gets replaced before it has ever been ticked. `client_commit()` classified that
as a RETARGET — on `c->animation.running` alone — and a retarget anchors its
clock at `last_sample_ns`, which for an unsampled segment still holds the last
tick of the *previous, long-finished* animation. The new slide therefore started
however long the window had been sitting still ago. Measured on a 600ms slide:
first tick at `t_ms=1062`, `lin=1.0`, window at its target in one frame, no
slide drawn. A shorter idle gave `t_ms=465` — the curve starts 78% along, which
is what "falls short" was. 32% of slides teleported.

A retarget is a segment that was *interrupted*, not merely one that was
replaced, so `retargeting` now also requires `last_sample_ns > time_started_ns`.

**Why geometry could never have found it.** A slide that never plays leaves
every window exactly where a slide that played perfectly leaves it. Three of the
four failed hypotheses were geometry hypotheses. The animation CLOCK is the only
witness: `AZ_PACE=1`, take each `anim start action=4` with a non-zero span, and
read `t_ms` on its first `anim tick`. A few ms is healthy; hundreds is the bug.

### Falsified along the way — do not re-derive

1. **Character-cell quantisation.** kitty's buffer is pixel-exact for its box.
2. **The animation start position.** Two fixes written against
   `set_tagin_animation()`'s off-screen `animainit_geom`; neither changed
   anything, both reverted.
3. **A corrupted `c->geom`.** `resize()` instrumented for `geo.x < -100`; never
   fired once.
4. **`set_tagin_animation()`'s `running` early-return.** The leading suspect for
   a day. It is not wrong — it hands the in-flight position to the new segment,
   which is right. What was wrong is the clock that position was pinned to.
5. **The first reproduction itself.** It put windows on a SCROLLER tag and read
   x=-1480 as a stranding, when a scroller parks windows off-strip by design.

### Still open, benign

A tag switch emits two `anim start`s per client one frame apart (83 of 250 in a
20-round-trip run were superseded before their first tick). Harmless now that a
replaced-but-unsampled segment restarts cleanly, but the second arrange is
redundant work.

## Spring duration is coupled to spring frequency

Not a defect, and documented in `contrib/anim-pace-test.sh`'s header, but it
surprises people and it is what "the animation falls short" turned out to mean.

The spring is evaluated over NORMALISED time, so `frequency` says how many
radians it travels per configured duration, not per second. Measured with
`frequency 10`: every MOVE animation configured for 500ms completed in ~202ms
(`closed=completed`, `dead_tail` ~0) — the spring converges at t≈0.4 and stops.
An earlier measurement at `frequency 22` finished in 23% of its duration.

`duration` is therefore an upper bound the spring may never reach. The lever is
`frequency`; roughly 4-5 keeps it in motion for most of the configured time.
