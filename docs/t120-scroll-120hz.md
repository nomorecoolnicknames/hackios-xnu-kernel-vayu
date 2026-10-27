# 120 Hz while a finger is down

`fb_120=1` makes the panel's clock and CoreAnimation's animation clock run at 120: a menu
transition moves at 120 a second. A scroll that a finger drives does not, and `t120=1` is the flag
for the rest of it. This note records what was found in the 20F66 system (iPhone10,3 identity,
iOS 16.5) and what the two flags do; it is `FACT` where an address or a log line is given, and
`HYPOTHESIS` where a run is still needed.

## What already works

* The panel runs at 120.0 Hz and the display driver says so (`vayu-vsync: 2: after 30 s: 3600
  frames counted`), and with `fb_120=1` the mode is published as 120 (`SyncRate = 120 << 16`).
* `<QuartzCore>` `CADisableMinimumFrameDuration` / `...OnPhone` are answered with `true` for
  backboardd and SpringBoard (`src/inject.c`, `src/inject_sb.c`) and for the applications in the
  default `INJECT` list (`src/inject_app.c`). In 20F66 the two keys are the CFStrings at
  `0x1d5b6b198` and `0x1d5b6b1b8`, read at `0x1884c2d70` and `0x1884c2e2c`; the answer of the second
  one is cached in the byte at `0x1dcf3fe28` (`strb w0, [x8, #3624]` at `0x1884c2e44`), which the
  display-link code tests at `0x18828b27c`.

## What still holds a finger-driven scroll at 60

`<QuartzCore>` keeps the display's frame-rate range under its own control, and it does not take
this display for a ProMotion one. It is not an Info.plist key and not a MobileGestalt answer:

* The switch names are 207 C strings at `0x1d5b57720` ("CA_COLOR_FLUSH" first), and the answers are
  bytes at `0x1e1f6b790`, filled from the environment in one loop at `0x188370a90` (`cmp x23, #153`).
* `113` is `CA_ENABLE_ALL_FRAME_RATES` (`0x1882cea0c`), `118` is
  `CA_ENABLE_HIGH_FRAME_RATE_RESTRICTION` (`0x1883ac018`; its getter `0x1883abfec` returns
  `(byte 0x1e1f6a1e8 != 0) | settings[118]`, and both bytes are 0 in the cache, so this switch is
  off by default and the 60 comes from the range computation, not from it), and `119` is
  `CA_FORCE_120HZ`, byte
  `0x1e1f6b807`, read at `0x18828b294` and `0x188306184`, both in the display's frame-rate
  arbitration; the flag skips the clamp block there. With `119` set, the branch at `0x18828b328`
  stores `0x42f00000`, which is 120.0f, into the three floats of a `CAFrameRateRange`
  (`str d0, [x20, #24]` is minimum and maximum, `str w8, [x20, #32]` is preferred): the switch does
  what its name says.
* backboardd's digitizer is delivered on a display link:
  `BKDigitizerEventSchedulerWithDisplayLink` (class `0x100100368`) creates it at `0x10000d9fc`
  (`displayLinkClass`) → `0x10000da0c` (`displayLinkWithTarget:selector:`) → `0x10000da38`
  (`addToRunLoop:forMode:`) → `0x10000da44` (`setPaused:`), and never calls
  `setPreferredFramesPerSecond:` -- the only such call in backboardd is
  `BKMousePointerAnimation`'s, `0x1000424e4`. A display link with no preferred rate runs at the
  display's maximum, which is the number the restriction above clamps.
* There is no ProMotion MobileGestalt key to answer: the string `DeviceSupports120Hz` does not
  occur anywhere in the 20F66 cache, and `libMobileGestalt`'s display keys are `main-screen-scale`,
  `-width`, `-height`, `-pitch`, `-orientation`, `display-rotation`, `display-scale`,
  `display-corner-radius`. The MobileGestalt route is a dead end for this.

## The flag

`t120=1` (boot argument, independent of `fb_120=1`; give both) does, in every process that has a
library of ours:

1. `setenv` `CA_FORCE_120HZ=1`, `CA_ENABLE_ALL_FRAME_RATES=1`,
   `CA_ENABLE_HIGH_FRAME_RATE_RESTRICTION=0`; and
2. writes those three bytes in QuartzCore directly (`src/inject.c` and the two other libraries),
   after checking the image's uuid (`dc2dd4c1-7866-3443-8050-fc915a0204c1`) and that
   `CA_FORCE_120HZ` is at `0x18859555b`, and again after 2 s and 10 s in case QuartzCore fills its
   table from the environment after the constructor; and
3. writes `1` into the cached `CADisableMinimumFrameDurationOnPhone` answer at `0x1dcf3fe28`, so a
   scroll does not depend on the bundle lookup having happened before the display link was made;
4. in applications and SpringBoard, answers `-[UIScreen maximumFramesPerSecond]` (and the private
   `_maximumFramesPerSecond`, and `-[CADisplay maximumFramesPerSecond]` if it exists) with 120, so
   UIKit's `CAFrameRateRange`/`preferredFrameRateRangeMaximum` is built around 120; and
5. refuses the event server's `ReportInterval = 0` for the digitizer, which had left
   `BKIOHIDService` with `reportInterval:0` at attach (TEST3, 21:50:21.092).

Nothing is done without the boot argument.

## What a run should show

* `hakios: t120=1: QuartzCore's frame-rate restriction is off ...` in backboardd, SpringBoard and
  the application that is scrolled (Settings is the easy one).
* `hakios: t120=1: -[UIScreen maximumFramesPerSecond] answers 120` in the application.
* `hakios: input: t120=1: the event server's ReportInterval 0 refused; it stays 4166 us`.
* The frame counter of the display driver (`fb_120: N refreshes told ...`) is not the test; the
  test is `MiniIOMobileFramebuffer: M frames in 5000 ms (F a second)` while a finger is on the
  screen, and F should be well over 60 when the scroll is fast.
* If F stays at 60 with all five lines present, the restriction is not the limiter: the next thing
  to look at is the display link's own rate in backboardd (`BKDisplayLink`, `0x100101ab0`), and
  whether its `CADisplay` is the main one.
