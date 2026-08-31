---
title: History
description: How asteroidz got its own renderer, in one page.
---

# History

asteroidz is a fork of [mango](https://github.com/mangowm/mango), itself a fork
of [dwl](https://codeberg.org/dwl/dwl/). What it added over that lineage is a
Vulkan renderer of its own — **AVK** — and the colour, presentation and effects
work built on it, delivered as milestones M1 through M13.

This page is what remains of roughly 16,000 lines of milestone documents: the
ADRs live in [decisions](/docs/decisions) because the code cites them, the traps
worth repeating are below, and the narrative is in the git history where it
belongs. Milestones were retired at M14. They were useful while the renderer was
being built out of nothing and stopped being useful once it worked; what
replaced them is ordinary work with tests attached.

## What each milestone delivered

| | |
|---|---|
| **M1–M3** | Vulkan core, image and DMA-BUF import, native scene submission. The renderer stops being a SceneFX consumer and starts owning the frame. |
| **M4** | Effects as first-class primitives: borders as one primitive, gradients with defined reference semantics, analytic shadows, a render graph with a barrier compiler and a transient pool, and blur — the largest single piece, and the source of most of what follows. |
| **M5** | Scene-linear colour. The working space, per-window luminance domains, the two output paths, tone and gamut mapping, dither. ADR-000 through ADR-013. |
| **M6A** | Presentation ownership: the per-output presenter, the two-regime predictor, one sample instant per pass. ADR-600 through ADR-617. |
| **M6B/M6C** | Colour management — ICC ingest, the matrix-shaper path, and the 65³ cube for profiles that do not reduce to a matrix and a curve. Owning the colour protocols rather than proxying them. |
| **M7–M13** | Consolidation: occlusion culling, direct scanout, tearing, the damage contract under transforms, and the surface-intent inspector that answers "what is this window and what are we doing to it". |
| **M14** | Recording and screencasting. Current work — see [the M14 plan](/docs/m14). |

## What was learned, that is still true

These outlived their milestones. Most were paid for twice before being written
down.

**A test that cannot fail is worse than no test.** Repeatedly: fixtures that
passed because a pointer call was silent, because a break flag was never
forwarded through `env -i`, because the binary under test was not the binary
built. Run every new test against the broken build *first*, and assert the
premise — a premise that can never hold makes every assertion under it free.

**Absence of evidence reads exactly like evidence of absence.** A missing JSON
key compares as non-zero in every shell. A histogram of an empty region has
leftover extents. A counter that is zero because nothing ran looks identical to
one that is zero because nothing went wrong. Say which.

**A number that produces a confident wrong conclusion is worse than no number.**
`vblanks_per_frame` divided a client's commit interval by the panel's scan rate
and read 4.5 on content that was being presented perfectly. It was believed for
a while.

**Instrumentation is not free and is not permanent.** Every counter added to
settle a question should leave when the question is settled, or acquire a test
that reads it. Both `get avk-stats` and `get presentation` had to be cut back
hard once this was ignored for long enough.

**Report the state, not the request.** `hdr` reported what we asked for and read
`true` while the panel was in SDR; `bitdepth` reported our own buffer format;
`output_vrr_active` reported that a state *test* had passed. Where the hardware
can be asked, ask it, and where it cannot, name the field so it does not claim
more than it knows.

**The experiment that identifies a cause is not the experiment that verifies the
fix.** Turning a feature off proves what it was responsible for. Only turning it
back on tests the fix.

**Flat test content is blind to whole classes of defect.** A blur of a flat
backdrop cannot show that the blur raised the mean; an opaque test window cannot
show a shadow's exclusion fill bleeding through a translucent one. Use
high-frequency content and translucent windows.

**A fresh buffer per frame is a fresh GPU import per frame.** The compositor's
own cairo producers hand the scene a new `wlr_buffer` on every update, and each
one AVK sees becomes an imported image with its own memory, view and descriptor
set. A close animation drawing ten fragments this way left about 115 of them
behind per window. Redrawing one buffer in place is safe for these — they have
no dma-buf and no shm fd, so the upload is always synchronous on the same
thread — as long as the scene is told with
`wlr_scene_buffer_set_buffer_with_damage()`, which is what drives the content
hook AVK re-uploads on. `text-node.c` and `ufo-node.c` still allocate per
update.

**A one-window fixture allocates nothing, so it can prove nothing about
allocation.** pixman keeps a single rectangle inside the region struct, so
copying one-rect damage never touches the heap. A per-frame region that was
never finished leaked a gigabyte of resident memory in three and a half days on
the desktop, three objects in five minutes against one window, and not one byte
against an idle output — same binary, same code path. A leak fixture needs two
outputs, two drawing clients and the real config's effects, and the oracle is
the sanitiser's report rather than RSS: the ASan allocator ramps 130 MB in three
minutes and buries what you came to measure.

**GPU occupancy is not GPU load**, and a "2.6× faster warm GPU" result was
unequal animation counts rather than physics. Match the workload before
comparing the numbers.
