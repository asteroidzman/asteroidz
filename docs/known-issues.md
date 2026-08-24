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

## OPEN — a tag switch arranges twice

A tag switch emits two `anim start`s per client one frame apart (83 of 250 in a
20-round-trip run were superseded before their first tick). Harmless now that a
replaced-but-unsampled segment restarts cleanly, but the second arrange is
redundant work.
