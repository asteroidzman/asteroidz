# Known issues

Open defects with what has been established and, as importantly, what has been
*ruled out*. The point of this file is that the next attempt starts where the
last one stopped instead of re-deriving it.

## M14 — what a capture client is actually offered

Measured 2026-08-20 with `contrib/wlcapture`, the ext-image-copy-capture client
M14's audit found was missing. Recorded here because the numbers change what M14
has to build — twice, in opposite directions.

**The live HDR output offers 10 bits per channel.** DP-1, `hdr_enabled=true`:

```
output      DP-1
buffer      3840x2160
shm formats 1
  shm       XRGB2101010    10 bpc
dmabuf formats 3
  dmabuf    XRGB2101010    10 bpc
  dmabuf    ARGB8888       8 bpc
  dmabuf    XRGB8888       8 bpc
shm depth   10 bpc max
```

So a capture client asking for shm on this output gets `XRGB2101010` at the
panel's full 3840x2160 — not the 2560x1440 logical size, the physical one. The
10-bit path already reaches capture clients, today, with no compositor change.

**The headless output offers 8, and that nearly became a false conclusion.**
The same tool against a headless 1920x1080 output reports exactly one shm format,
`XRGB8888`, and two 8 bpc dmabuf formats. Read alone that says "the compositor
advertises nothing above 8 bpc, which is why portal capture is SDR" — and it is
wrong. It is a property of an SDR headless output, not of the compositor. The
headless backend also refuses HDR outright (it reverts `m->hdr` on the next
commit, see `contrib/regression/tests/hdr.sh`), so this question cannot be
answered headlessly at all, and an 8 bpc reading there is not evidence about
anything else.

**So "portal capture forces the output down to SDR" is not the compositor
advertising 8 bpc.** Whatever forces it happens above this layer — in
xdg-desktop-portal / pipewire's own negotiation — and that is where it has to be
chased. The 2026-07-19 live observation stands; its cause does not.

**Throughput is not the problem either.** With a client repainting every frame,
`wlcapture` took 30 frames in 1.64s headless — **17.7 fps**, against
`hdr-record.sh`'s 1 fps. ext-image-copy-capture is already a working stream. The
1 fps measures the `screenshot_ui` freeze-and-read-back path, exactly as the
audit said, and is not a ceiling on capture. (17.7 fps is a floor: worst gap
101ms, and the repainting client is the likely pacer.)

**The offer is wlroots', not ours.** `asteroidz.c:11598` calls
`wlr_ext_output_image_capture_source_manager_v1_create()`, and
`wlr_ext_image_capture_source_v1` carries `shm_formats` and `dmabuf_formats` as
its own fields. asteroidz's whole involvement in a capture session today is
`handle_image_copy_capture_new_session()`, which counts sessions for the privacy
shield. D6's "sessions backed by AVK stage taps" therefore means implementing a
capture source rather than registering wlroots' — but the reason to do it is now
the *named stages and metadata*, not bit depth, because bit depth already works.

**Frames pull live, at rate, without disturbing the display.** 60 frames off
DP-1 in 3.62s — **16.3 fps** at 3840x2160 in `XRGB2101010`, no failures. The
compositor log grew by **zero bytes** across the run and `hdr_enabled` was still
true afterwards: no HDR transition, no modeset, no retrain. That is the opposite
of `grim` on this output, which forces HDR off plus two modesets and a failed
commit per capture.

The pixels are genuinely 10-bit, not 8-bit widened. In one frame's red channel:
536 distinct values, of which **401 are not multiples of 4**, and the low two
bits are near-uniformly distributed (2.07M / 1.93M / 2.11M / 2.19M pixels across
the four buckets). An 8-bit source shifted left by two would put every value on
a multiple of 4 and leave those buckets empty. Range 0..629 of 1023, consistent
with a PQ encoding whose desktop content sits well below peak. Decoded to a
preview the frame is the desktop, right way up, right pitch, right channel
order.

**So `hdr-record.sh`'s reason for being is gone.** It records at 1 fps because
`screenshot_ui,rawhdr` freezes the output per call. The same output will hand a
client 10-bit HDR frames at sixteen times that rate, through a protocol that has
been wired up the whole time and had no client to ask it. Replacing that script's
source is a contrib change, not a compositor one.

**One thing a recorder must handle:** consecutive captures can return identical
content — frames 1 and 2 of a five-frame run differed by 0 pixels, the others by
~100-150 (a clock and a cursor on an otherwise static desktop). Capture completes
on presentation, so a duplicate means nothing new was presented; pacing has to
come from `presentation_time`, not from counting `ready` events.

## M14A — the encoder takes AVK's own image, unconverted

Settled 2026-08-20 with `asteroidz-avk-probe`, which now asks the driver instead
of reading profile names out of `vulkaninfo`.

RADV on Navi 31, encode queue on family 3:

| | AV1 Main | H.265 Main 10 |
|---|---|---|
| max extent | 8192x4352 | 8192x4352 |
| DPB slots | 9 | 17 |
| picture alignment | 64x16 | 64x16 |
| rate control | disabled, CBR, VBR | disabled, CBR, VBR |

3840x2160 is inside the limit and lands exactly on the 64x16 alignment, so this
display needs no padding pass.

**The input format depends entirely on asking for the conversion.** Queried
plainly, both codecs accept `P010` — a 10-bit YUV plane pair — and nothing else.
Chain `VkVideoEncodeProfileRgbConversionInfoVALVE` with
`performEncodeRgbConversion = VK_TRUE` into the *profile* and the same query
answers:

```
rgb models: BT.2020 BT.709   ranges: full narrow
input picture  A2B10G10R10 (RGB 10-bit)  optimal, linear, modifier
input picture  A2R10G10B10 (RGB 10-bit)  optimal, linear, modifier
```

`A2R10G10B10_UNORM_PACK32` is `DRM_FORMAT_XRGB2101010` — the format AVK already
renders an HDR output in, and the one DP-1's capture session already offers. So
the encoder will take the composited image exactly as it stands: no readback, no
dmabuf export, no RGB->YUV pass of ours, and the BT.2020 model HDR10 requires is
one the driver performs itself.

That is the whole architectural case for putting capture inside the compositor,
and it now rests on a measurement rather than on an extension being present in a
list. It is also a *profile* property, not a session flag: an encoder that
converts is a different video profile from one that does not, and the format
query answers differently for each.

**A trap worth keeping.** `vkGetPhysicalDeviceVideoCapabilitiesKHR` segfaults on
RADV if the codec-specific capabilities struct is missing from the `pNext`
chain. Not an error return -- a crash, one line after the encode queue is
reported.

## FIXED — M14A: a correct picture that decoded as noise

Resolved 2026-08-21. The encoder was always reading its source and always
encoding it correctly; the parameter sets described a stream that could not
exist, and the decoder was left to reconcile them.

**The cause.** `VkVideoEncodeH265CapabilitiesKHR.maxLevelIdc` is **0** on this
driver -- `STD_VIDEO_H265_LEVEL_IDC_1_0`, whose limit is 176x144 -- and it was
copied straight into `general_level_idc`. Every 3840x2160 stream therefore
declared level 1.0. Nothing rejects that: the encoder encodes, the headers are
written, `ffprobe` reports `hevc / Main 10 / 3840x2160 / yuv420p10le`, and only
the decoded samples are wrong. The level is derived from the luma sample count
now. A second inconsistency went with it: `max_transform_hierarchy_depth` of 0
asks for a TU tree that cannot describe the 64x64 CTB the encoder uses.

**Verified end to end.** A source cleared to RGB (0.25, 0.55, 0.85), encoded and
decoded, comes back as Y 500 (std 0.62), Cb 706, Cr 346 against the 500/708/346
that BT.2020 non-constant luminance at full range predicts -- exact to the
quantiser at QP 26.

```sh
AVK_ENCODE_OUT=/tmp/s.h265 ./build/test-avk-encode
ffmpeg -i /tmp/s.h265 -frames:v 1 -pix_fmt yuv420p10le -f rawvideo /tmp/s.yuv
```

**Two wrong turns worth keeping, because both were reasoning errors rather than
coding ones.** The first: "the output is identical across two unrelated inputs,
so the encoder ignores its source." Both runs cleared the source to the *same
colour* -- identical output was exactly what a working encoder should produce.
The bisect that settled it was one run with a different colour, and it took
seconds. The second: the same class of mistake earlier, blaming the RGB input
path on a stale test binary, because `test-avk-encode` is
`build_by_default : false` and a plain `ninja -C build` never rebuilt it.

**The metadata followed.** The SPS now carries a VUI, so the stream says what it
is instead of leaving a player to guess:

```
codec_name=hevc      profile=Main 10      level=153
width=3840           height=2160          pix_fmt=yuv420p10le
color_range=pc       color_space=bt2020nc
color_transfer=smpte2084                  color_primaries=bt2020
```

Round trip: RGB (0.25, 0.55, 0.85) in, (0.2483, 0.5487, 0.8437) out -- within
0.006, which is 10-bit quantisation plus 4:2:0 chroma at QP 26. That is only
exact because the decoder inverted the BT.2020 matrix the shader applied, which
is why the matrix coefficients are push constants derived from the same
`enum avk_encode_colour` that writes the colour description. A picture converted
with one matrix and labelled with another is not obviously broken -- it is
slightly wrong in the greens, and that survives review.

**All three shipped.** The SEI, a HEIF container, and `amsg dispatch
screenshot_hdr`, which arms every output currently running HDR and writes
`~/Pictures/Screenshots/hdr_<output>_<timestamp>.heic` from the next finished
frame. The encoder copies the scanout attachment into an image of its own
because a colour attachment carries no STORAGE usage, converts, encodes and
wraps -- and the picture never reaches the CPU on the way.

An SDR output is declined rather than captured: its attachment is 8-bit, and a
file claiming BT.2100 PQ over it would be worse than no file. The luminance in
the mastering SEI is the display's own, from EDID; the primaries are BT.2020
reference values, because nothing in this project exposes a panel's own
chromaticity points -- the same fallback `hdr-record.sh` makes, and the standard
one when a display's own are unavailable.

**Not yet verified on real content.** Everything above is proven against a
cleared test image and a headless run that correctly declines. Whether a real
composited HDR desktop encodes to a picture that looks right needs the live
display.

## M14B — a sequence, and the test that could not see it was broken

Added 2026-08-21. The encoder does video now: an IDR followed by P pictures
predicting from the one before, with a two-layer DPB whose slot index and array
layer are deliberately the same number. `I P P P P P P P P P`, 6226 bytes for
ten 1080p frames against ~2700 for a single still, decoded luma ramping
429 -> 594 in exactly the order encoded.

**The bug worth keeping.** The first working version scored **24/24** on
`tests/test-avk-encode.c` and decoded to **one frame**. `RefPicList0[0]` was set
to the DPB slot index, which tells the ENCODER what to predict from and says
nothing to a decoder -- the bitstream needs a short-term reference picture set
in the slice header, and `pShortTermRefPicSet` was NULL. ffmpeg: *"zero refs for
a frame with P or B slices"*, nine pictures dropped.

Every property the C test could reach was correct. The NAL units were correctly
typed, correctly ordered, one IDR and nine P frames, parameter sets on the first
and not repeated after. The stream was undecodable.

So `contrib/avk-encode-test.sh` exists beside it and its oracle is a decoder,
not the bitstream's shape. It asserts what comes OUT: the still decodes to the
colour that went in and is flat, the sequence decodes to ten pictures, and their
luma ramps in the order encoded. Falsified against the build with
`pShortTermRefPicSet` removed -- the unit test still says 24/24, the fixture
reports three failures.

**The container and the loop followed.** `avk_mp4` writes a real MP4 -- sample
data appended as it arrives, sample table held (sixteen bytes a frame), index
written at close. `amsg dispatch record_start` / `record_stop` record the
focused HDR output to `~/Videos/asteroidz_<output>_<timestamp>.mp4`.

**What it costs, stated rather than hidden.** Every frame is waited for and
encoded on the compositor's own thread, so recording makes frames later than not
recording does. `record_stop` reports frames captured and dropped, which is the
measurement that decides whether an asynchronous encode is worth building --
that number is not yet known, because the path needs an HDR output and the
headless backend refuses HDR.

**Sample durations are measured, not assumed.** A compositor renders when
something changed, not on a cadence, so a recording timed at the output's
nominal refresh plays back at the wrong speed whenever the desktop was idle.
Each sample's duration is the interval since the previous frame, floored at 1ms
and capped at 1s.

**The file is unplayable until `record_stop`,** because an MP4's index lives at
its end. An output torn down mid-recording -- unplugged, or the compositor
exiting -- closes the file where it stands rather than abandoning it, so the
worst case is a short recording and not an unopenable one. A compositor that is
killed outright is still a lost file.

**Not yet exercised on a real output.** The encoder and container are covered by
`contrib/avk-encode-test.sh` against synthetic frames; the compositor glue that
drives them is not, for the same reason `screenshot_hdr` is not -- it needs HDR,
and that only exists live.

**Still to build:** rate control other than fixed QP (a recording currently has
no bitrate target at all), and periodic key frames so a long recording is
seekable.

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

**The lesson, which cost more than the bug.** Every test in the tree encoded a
FLAT COLOUR. A flat picture has almost no residual, so a syntax desync has
almost nothing to corrupt: the suite scored 24/24, ffprobe agreed on every
header, and the first real desktop was green. `tests/test-avk-encode.c` now
uploads a high-frequency pattern that moves each frame, and
`contrib/avk-encode-test.sh` asserts the decoded detail survives -- luma std
215 against a flat field's 0.6. This is the same trap
`docs/known-issues.md` already records for the shadow tests, in a new place.

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
