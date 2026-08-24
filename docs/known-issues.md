# Known issues

Open defects with what has been established and, as importantly, what has been
*ruled out*. The point of this file is that the next attempt starts where the
last one stopped instead of re-deriving it.

## OPEN — the encode session is not in the rate control mode it thinks

Found 2026-08-24 with the validation layers on, while closing M14B. Two VUIDs,
once per picture:

    VUID-vkCmdBeginVideoCodingKHR-pBeginInfo-08253
    VUID-vkCmdEncodeVideoKHR-constantQp-08272

`submit_picture()` sets `VK_VIDEO_ENCODE_RATE_CONTROL_MODE_DISABLED_BIT_KHR`
exactly once, in the `if (!enc->session_reset)` branch, and never chains
`VkVideoEncodeRateControlInfoKHR` into `VkVideoBeginCodingInfoKHR`. The spec
says that if BeginCoding does not carry that structure then the mode in force
for the scope must be DEFAULT -- so every coding scope runs as DEFAULT, not as
DISABLED. And under DEFAULT `constantQp` must be zero, while we pass 26.

So the QP the code believes it is encoding at is not a QP the API agreed to.
RADV appears to honour it anyway (sweeping it does move the picture size
monotonically), which is why nothing has looked wrong. Fix shape: chain the
rate control info into every BeginCoding rather than setting it once.

Not the cause of M14B -- it is present identically in the broken and the fixed
build -- but it is a real contract error and it makes every statement about
"QP 26" in this subsystem provisional.

## OPEN — a recording is one IDR and an unbounded chain of P pictures

`az_avk_record_frame()` is the only caller of `avk_encoder_encode_frame()` and
passes `force_key = false` unconditionally; the only key frame a recording ever
contains is frame 0, because `key` is `force_key || frame_index == 0 ||
all_intra`. There is no IDR period anywhere in the encoder or the recorder.

A file like that cannot be seeked and has no recovery point: any picture that
is lost or malformed has no later IDR to resynchronise on.

This is why `AZ_ENCODE_INTER` still exists and all-intra is still the default
even though M14B's corruption is fixed: inter prediction now produces correct
pictures (11.7x smaller than all-intra on correlated content), but a recording
made of one IDR and a thousand P pictures is not a file anybody should be given
until it has recovery points. Settle this and the rate control entry above,
then promote inter and delete the switch.

NOTE: it was tempting to say this is what turned the rare M14B corruption into
a wholly green file, via error propagation down the P chain. **That was tested
and not confirmed**: with unrelated photos only the corrupt frame is wrong and
the P pictures after it recover, because the encoder codes them as effectively
intra. Propagation on correlated desktop content is plausible and unproven.

## OPEN — a P slice claims a reference count it does not set

`submit_picture()` sets `num_ref_idx_active_override_flag = 1` for every P
slice, with the comment "A P slice states its own reference count rather than
inheriting the PPS default". The driver emits **0**, and the comment is
therefore false: the slice does inherit the PPS default. Harmless, because the
PPS default and our own `num_ref_idx_l0_active_minus1` both yield exactly one
active L0 reference, so the effective count agrees either way.

Recorded as false authority to remove rather than as a defect: it is the only
place where what this code submits and what the driver emits disagree, and it
cost time during M14B precisely because it looked like it might be the answer.

## OPEN — a tag switch arranges twice

A tag switch emits two `anim start`s per client one frame apart (83 of 250 in a
20-round-trip run were superseded before their first tick). Harmless now that a
replaced-but-unsampled segment restarts cleanly, but the second arrange is
redundant work.
