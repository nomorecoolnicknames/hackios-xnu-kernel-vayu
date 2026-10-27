# Where a finger-driven scroll loses its frames (T120b)

`t120=1` lifted QuartzCore's frame-rate restriction and made `UIScreen` answer 120, and SpringBoard's
own animations do run at 119 a second on the phone (TEST9: 597 frames in 5004 ms). A scroll inside an
application still comes in at 60–74. This note is the measurement of where those frames go, from the
five-second lines of the display driver in the TEST9 run
(`artifacts/vayu/test9-20261027/run/135747-merged-dedup.log`), and the four flags that come out of
it. `FACT` where a log line is quoted, `INFERENCE` where the number is read, `HYPOTHESIS` where a run
is still needed.

## The frame's budget

Every five seconds, and only in a window in which something was drawn, the driver says:

```
MiniIOMobileFramebuffer: N frames in M ms (F a second), D us each in the driver, after G us waiting
for the GPU and, B of them, W us for the next refresh; H swaps waited for the frame before, L gave
up; S swaps came a refresh late themselves
MiniIOMobileFramebuffer: fences: X swaps came with a submission of the GPU's (the last was waited
for: T us of X, O of them 20 ms or more); ...
```

`D` is the driver's own work on the frame after the beat (the copy, the recording, the accounting),
`T/X` the average wait for the GPU's fence, `W·B/N` the average time given to the beat. Their sum is
the driver's pipeline for one frame; the panel's period is `M/N`.

| log line | frames | ms | fps | driver µs | fence µs | beat µs | held | late | period µs | pipeline µs | pipeline/period |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 26187 | 597 | 5004 | **119** | 1359 | 1881 | 998 | 0 | 14 | 8382 | 4238 | 0.51 |
| 26389 | 476 | 5015 | 94 | 1239 | 2805 | 1448 | 30 | 65 | 10536 | 5492 | 0.52 |
| 26003 | 374 | 5008 | 74 | 1306 | 3855 | 692 | 21 | 57 | 13390 | 5854 | 0.44 |
| 22185 | 377 | 5187 | 72 | 1384 | 3098 | 1115 | 30 | 104 | 13759 | 5597 | 0.41 |
| 25657 | 346 | 5000 | **69** | 1377 | 4473 | 990 | 62 | 82 | 14451 | 6840 | 0.47 |
| 26495 | 303 | 5000 | **60** | 1567 | 4725 | 1019 | 37 | 142 | 16502 | 7312 | 0.44 |
| 23858 | 279 | 5010 | 55 | 1623 | 899 | 1075 | 0 | 120 | 17957 | 3597 | 0.20 |
| 24132 | 151 | 5007 | 30 | 2194 | 699 | 1238 | 0 | 151 | 33159 | 4130 | 0.12 |

The last row is the idle screen; the ones above it are a touch, a scene change, or a scroll.

**INFERENCE: the driver is not the ceiling.** In the 60–74 fps windows it is busy for 20–52 % of the
frame. A driver that set the rate would be at 100 % of it, or its queue (`fb_queue=2`) would be full
on every swap; it is full on 62 of 346 swaps in the 69 fps window, 0 of 597 in the 119 fps one.

**INFERENCE: the copy is not the ceiling.** It was, once: 9434/7816/9372 µs a frame when the picture
was scaled (`receipts/2026-10-24-vayu-drivers-mgr.md:1040`), which no 8.33 ms frame can hold. With
`fb_crop=1` (rows copied, not blended) and `fb_threads=6` it is 1032–2228 µs — 10.4 MB read and
10.4 MB written a frame, 4.7 to 10 GB/s each way (7.6 in the 119 fps window) — and it is 1359 µs there and
1377 µs in the 69 fps one. The same copy, twice the rate.

**INFERENCE: `fb_swap_fence_ms=20` is not a ceiling.** In 131 of the run's 132 `fences:` lines there is
no wait of 20 ms or more at all; the 132nd (the 60 fps window) has exactly one. The average wait is 0.7 ms idle and 1.9–4.7 ms
working: that is the GPU finishing the frame's submission, not a timeout being hit. A shorter
timeout would only show frames the core has not finished.

**INFERENCE: the 30 fps of an idle screen is not a divider in this driver.** The driver's own work
is 4.1 ms of each 33.2 ms, and in the same window it tells user space 605 refreshes in 5007 ms
(121 a second): the window server is told every panel frame and asks for one in four.

## What does hold it

The same run has a window at 119 fps with the same driver, the same copy and the same GPU fence
code, and the run's own renderer says what it costs: `hakios-gl: after 9600 frames: ... 2811 us a
frame` (backboardd, `src/eagl.c`) — 2.8 ms of window-server work against an 8.33 ms frame. So the
window server *can* be told 120 and *can* answer 120. In the slow windows it is told 100–120
refreshes a second and answers 60–74 (`fb_120: 499 refreshes told to user space in 5000 ms (100 a
second)` against 346 frames): the frames are not late, they are not asked for.

`HYPOTHESIS` (the one `t120_link` is for): the rate an application asks its own display link for is
still 60 for a scroll view, so the window server has nothing to composite at 120. UIKit builds the
range from `UIScreen.maximumFramesPerSecond` — answered 120 by `t120=1` — but a `CADisplayLink`
carries a range of its own, and whether `UIScrollView` sets it from the screen or keeps a 60 is what
the next run has to say. Falsifier: with `t120_link=1`, a scroll in Settings logs
`hakios: Preferences: t120_link: the range on a scroll view's link: the range 60..60, preferred 60`;
with `t120_link=2` the same scroll runs at 119 a second in the driver's counter.

Second `HYPOTHESIS` (the one `fb_queue`/`fb_early` are for): a swap held in `SwapEnd` makes the
window server miss its next tick. It is measured (`62 swaps waited for the frame before`) but it is
not the whole 60: with two queue places and a 9 ms pipeline the producer is held for at most one
period per frame.

## The flags

| flag | what it does |
|---|---|
| `fb_budget=0` | off: the per-frame accounting in `frameBudget()`. On by default; a few adds a frame. |
| `fb_queue=N` | 1 to 4 swaps may wait for the panel (default 2). 3 or 4 if the window server has that many display surfaces — the new line `the frames came in N different surfaces` says how many. |
| `fb_early=1` | the swap is let go of in `show()` before the waits for the GPU and the beat, not after them: one more frame in flight, and `SwapEnd` is never held behind a fence. Needs `fb_queue + 1` display surfaces. |
| `t120_link=N` | in an application, with `t120=1`: 1 says what a `CADisplayLink` is asked for (the first 8 in a process); 2 also answers a scroll view's link 120 where it asked for 60 or more; 3 does that for every link that asked for 60 or more. |

The five-second lines to read first, after a scroll:

```
MiniIOMobileFramebuffer: the frames that came late: A of B over 12 ms; of those, C waited over 4 ms
for the GPU's fence, D spent over 3 ms in the copy, E neither
MiniIOMobileFramebuffer: the fence wait: ... ; the copy: ... ; the surfaces' own fences: N us a frame
MiniIOMobileFramebuffer: the frames came in N different surfaces; in that time M surfaces of all
kinds were made, in X us each, and K freed, in Y us each
```

`E` (neither) is the number that decides: those frames nothing in this driver and nothing in the GPU
held up. If `E` is most of `A`, the rate is the window server's, and `t120_link` — not the
framebuffer — is where the next change goes.

## Why zero-copy scanout is not in this set

`fb_zero=1` (the panel scanning an IOSurface of the scanout pool directly, no copy at all) is the
structural way to take the copy off the frame, and it is written. It needs the loader's `fb_spare`
frames and a VIG0 flip, and that path hung the phone twice with no log
(`receipts/2026-10-23-vayu-backup-fw-hwruns.md:281`, `:317`: `fb_spare=2 vayu_mdp=2 vayu_mdp_flip=0`
hung). Nothing here changes that: it stays behind its own flag and its own gate, and the measurement
above says the copy is not what the 60 is made of, so it is not the first thing to try.
