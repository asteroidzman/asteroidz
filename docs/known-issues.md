# Known issues

Open defects with what has been established and, as importantly, what has been
*ruled out*. The point of this file is that the next attempt starts where the
last one stopped instead of re-deriving it.

## OPEN — the validation session can segfault at exit after a capture overlay

`asteroidz-avk-debug` only. After a session that has both put the capture
overlay up and run a recording, `vkDestroyDevice` faults **inside**
`libVkLayer_khronos_validation.so`, having first reported
`VUID-vkDestroyImage-image-parameter: Invalid VkImage Object` and, twice,
`UNASSIGNED-Threading-Info: Couldn't find VkImage Object` for the same handle.

Same binary, same sequence:

    validation layer loaded      ~30% of runs SIGSEGV (1/8, 2/5, 4/5, 3/3)
    validation layer NOT loaded  0 of 10
    ASan, layer on and off       0 of 12, no report

Each half of the workload alone is clean: the overlay with no recording is 0/6,
a recording with no overlay is 0/6. It takes both.

WHAT WAS RULED OUT, so nobody re-derives it. The image the layer rejects is a
swapchain target imported from a dma-buf; it is allocated once and destroyed
once -- every `vkDestroyImage` call site in the tree was logged and no handle
appears twice. `avk_image_destroy`'s own double-destroy guard never fires, the
retire queue's duplicate-push guard never fires, and the live-object ledger
reads zero for every class at `vkDestroyDevice`.

So the app's destroy accounting is balanced and the object the layer cannot
find is one the app destroyed exactly once. Whether the layer is losing it or
detecting something neither ASan nor AVK's own guards can see is NOT settled --
what is settled is that it needs the layer, so it cannot reach a session that
does not load one. `asteroidz-avk` has no `ASTEROIDZ_VK_DEBUG`.

The consequence for acceptance runs: a teardown check under validation must
read the process exit status, not the log. A run that faults here still writes
every stat line and then dies, so grepping for `AVK_TEARDOWN_END` reports a
crash as a pass.

## OPEN — a GPU reset ends the session; the device is not recreated

A GPU reset outside this process -- another client hanging the graphics ring,
which radv reports as *"the CS has been cancelled because the context is lost.
This context is innocent."* -- surfaces here as `VK_ERROR_DEVICE_LOST` on the
next submit. AVK cannot build a frame after that, and the session ends.

It ends *cleanly* now: the loss is recognised as a distinct state and answered
with `wl_display_terminate`, so clients are disconnected and teardown runs.
Before, it fell through as an ordinary submit failure into the abort that
exists for "AVK declined and nobody noticed", and produced a SIGABRT mid-frame
with the damage ring half rotated.

What is still missing is recovery. A reset loses VRAM, so every imported client
image, every pipeline and every cache belongs to a device that no longer
exists; rebuilding them means recreating the `VkDevice` and re-importing
everything, with clients asked to redraw. Until that exists, a reset costs the
session -- which is a smaller loss than it was, and still a loss.

## OPEN — a scanned-out fullscreen client records nothing

Found while validating inter on the live output. With mpv fullscreen and direct
scan-out active (`surface-intent` showed `scanout_frames=19141` on DP-1), a
35-second recording produced *"record: DP-1 captured no frames; discarding"* --
nothing was composited, so there was no frame for `az_avk_record_frame()` to
encode. The same content windowed recorded 944 frames.

Pre-existing and unrelated to the encoder: the recorder can only encode what the
compositor composites. But the only symptom is that one line at stop, after the
recording has already been thrown away, and scan-out is exactly what a fullscreen
video player gets. Either the recording should suppress scan-out on the output it
is capturing, or refusing should be loud at `record_start` rather than silent
until `record_stop`.

## OPEN — a tag switch arranges twice

A tag switch emits two `anim start`s per client one frame apart (83 of 250 in a
20-round-trip run were superseded before their first tick). Harmless now that a
replaced-but-unsampled segment restarts cleanly, but the second arrange is
redundant work.

## OPEN — CPU allocations remain at shutdown under LeakSanitizer

A headless run with a changing SHM client reports retained fontconfig/Pango
allocations and the scene-owned DMA-BUF format set allocated by
`wlr_scene_set_linux_dmabuf_capabilities()`. The unchanged 0.31.0 release and
the presentation-boundary fixes produced the same report: 630,374 bytes in
15,190 allocations on this system. The exact font-cache size is environment
dependent.

Both runs reached `CLEANUP_END`; AVK's GPU object counts were zero and Vulkan
validation reported no errors. ASan reported no invalid accesses in this test,
but LeakSanitizer made the process exit nonzero. This is a remaining teardown
issue, not a clean sanitizer result.
