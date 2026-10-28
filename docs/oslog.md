# The unified log, and how it reaches the run log

The bring-up has two logs that are not the same thing:

* the **console**, which is where the run log comes from: the loader's ring, the kernel's
  message buffer and everything a job writes to `/dev/console` (mkjobs.py gives every job it
  copies `StandardErrorPath=/dev/console`), and
* the **unified log** (`os_log`), which every process writes into the kernel's firehose and
  which `logd` persists under `/private/var/db/diagnostics`.

Only the first one ends up in a run log, so an application that taps out a line with
`os_log` says nothing that a run can show. `OSLOG="..."` in `build.sh` covers this a little:
`mkjobs.py` sets `OS_ACTIVITY_DT_MODE=YES` in the jobs it names, and libsystem_trace then
writes their `os_log` to their standard error. It cannot cover an application, because an
application's job is made by launchd/runningboardd from the bundle, not by `mkjobs.py`, and
its standard error is not the console.

`oslogfwd` (`src/oslogfwd.m`) is the missing reader: a root daemon that takes the live
stream of the unified log out of `diagnosticd` and writes the lines of the subsystems named
in a boot-arg onto the console.

## What is there, exactly

In this kernel (XNU 8796.121.2, `../kernel-vayu/xnu-src`):

| piece | where |
|---|---|
| `/dev/oslog` (a process gets its firehose buffer) | `bsd/kern/subr_log.c`, cdev slot 7 in `bsd/dev/arm64/conf.c:205` |
| `/dev/oslog_stream` (the reader; **one** at a time, a second `open` gets `EBUSY`) | `bsd/kern/subr_log_stream.c:959,983`, cdev slot 8 in `bsd/dev/arm64/conf.c:210` |
| the encoder and the queue behind both | `libkern/os/log_encode.c`, `libkern/os/log_queue.c` |
| `kern.bootargs`, which is how a boot-arg reaches user space | `bsd/kern/kern_sysctl.c:2163-2177` |

A run shows the devices and the daemons:

```
captures/vayu-20261025-sens2/195412-run.log:1489  crw-------  1  0 /dev/oslog
captures/vayu-20261025-sens2/195412-run.log:1490  crw-------  1  0 /dev/oslog_stream
captures/vayu-20261025-sens2/195412-run.log:2088  [ ok ] logd: os_log does not crash
captures/vayu-20261025-sens2/195412-run.log:2129  18 ?? 0:00.31 /usr/libexec/logd
```

`diagnosticd` is the daemon that owns `/dev/oslog_stream` and hands the stream to clients
(`allowing %s (%d) access to stream due to admin status` — root needs no entitlement; the
entitled path is `com.apple.private.logging.stream`). Its job declares the Mach services
`com.apple.diagnosticd`, `com.apple.syslog_relay` and `com.apple.os_trace_relay`, so launchd
starts it the first time something looks one of them up — it does not have to run from boot.

The client side is Apple's `LoggingSupport.framework`. It is in the iOS 16.5 dyld cache
(20F66, `/System/Library/PrivateFrameworks/LoggingSupport.framework/LoggingSupport`,
`libtrace-1431.100.13`), with the classes `OSLogEventLiveStore`, `OSLogEventLiveStream`,
`OSLogEventStreamBase`, `OSLogEventProxy`, `OSLogEventLiveSource`; the live stream's ivar
`_diagdconn`; and the strings `com.apple.diagnosticd`, `com.apple.os_trace_relay`, and
`BUG IN CLIENT OF LIBTRACE: OSLogEventLiveStream activated without stream handler`. That is
the same path `log stream` takes on a Mac. It is dlopen'd and not linked, so a system without
it leaves the daemon saying so instead of not starting.

## Using it

The filter is a boot-arg, because the run log is read from a run that is already over:

```
oslog=com.apple.amp.mediaplaybackcore,com.apple.UIKit:Scrolling
```

* `subsystem` selects the whole subsystem, `subsystem:category` one category of it.
* A trailing `*` is a prefix (`com.apple.amp.*`), and `all` / `none` are themselves.
* With no `oslog=` at all the list is the one a Music/Files/scroll investigation wants:
  `com.apple.amp.mediaplaybackcore`, `com.apple.amp.MediaPlayer`, `com.apple.FileProvider`,
  `com.apple.DocumentManager`, `com.apple.coremedia`, `com.apple.UIKit:Scrolling`,
  `com.apple.coreaudio`, `com.apple.avfaudio`.

Other boot-args: `oslog_proc=Music,Files` limits it to those process names, `oslog_rate=N`
is the messages a second one subsystem may have (default 10, `0` for no limit) and
`oslog_burst=N` the burst above it (default 20). A console write is a character at a time
through the loader, so the limit is the point: a chatty subsystem must not push everything
else out of the log. What the limit drops is counted and said once in a while.

```
XL_CMDLINE="-v oslog=com.apple.amp.mediaplaybackcore,com.apple.UIKit:Scrolling oslog_rate=20"
```

Each line carries the time, the process, the subsystem (and category), the level and the
composed message:

```
oslog 02:24:01.318 Music[412] com.apple.amp.mediaplaybackcore/Playback info: ... 
```

On the phone, `--selftest` says what the framework on this system actually answers to and
whether a stream could be created, and `--probe N` prints the first N events without the
filter and without the limit — the quick way to tell "the pipe is empty" from "the filter
matched nothing":

```
/redwood/bin/oslogfwd --selftest
/redwood/bin/oslogfwd --probe 20
```

## TEST13B: the stream opened and delivered nothing (X2)

`oslogfwd` ran, printed `streaming, backend OSLogEventLiveStream/initWithLiveSource`, and
five minutes later `alive, the stream has delivered nothing (0 seen)`. The reason was not
the filter and not the framework: the job was loaded into the **user/501 launchd domain**
and ran as uid 501 gid 501 (the plists in `/redwood/LaunchDaemons` say nothing about the
session, and launchd's daemon default is that domain), while `diagnosticd` answers a stream
request only when

* `__os_trace_peer_is_admin()` says yes (euid 0), **or**
* the client carries `com.apple.private.logging.stream`, **or**
* `com.apple.private.logging.stream.darwinos` on a darwinOS variant.

Anything else gets a reply `{error: 1}` and `xpc_connection_cancel()` — the code is at
`diagnosticd + 0x938c..0x95d8` (`__os_trace_peer_is_admin`, the entitlement lookup, the
`denying stream client (insufficient access)` message, the reply and the cancel). The binary
that ran had **no entitlements at all** (`ldid -S` with no plist: the signature's superblob
holds a CodeDirectory and requirements, no entitlements blob), so both halves of the check
said no. launchd did spawn `diagnosticd` on the stream's Mach lookup
(`Successfully spawned diagnosticd[185] because ipc (mach)`, 14 s after the backend line),
so the connection was made and then refused; libtrace invalidates the stream, but the old
code installed no invalidation handler and `-invalidated` is not a method of every stream
class, so it sat on it and the heartbeat kept saying zero.

The fix is in two places and both are wanted:

* `com.hakios.oslogfwd.plist` carries `LimitLoadToSessionType=System` (as
  `com.apple.logd.plist` and `com.apple.diagnosticd.plist` do), so the job is spawned by the
  system domain as uid 0; and
* `src/oslogfwd.entitlements` (`com.apple.private.logging.stream`,
  `com.apple.private.logging.diagnostic`) is now what `ldid` signs, so the job is served
  even where only the entitlement is checked.

`oslogfwd` also stopped taking chances with the rest of it:

* the subsystem predicate is still sent to `diagnosticd` first, because that is what keeps
  the XPC cheap (only the named subsystems cross it), but a minute without a single event
  drops it and reconnects without it: a predicate the framework's mapper inside the daemon
  cannot translate leaves a connected stream that delivers nothing, which cannot be told
  from a refusal. The filter in the daemon always decides what is printed; `oslog_pred=0`
  starts without the server-side predicate at all.
* the handler counts every call it is given (`n_events`), not only the events that have a
  subsystem, and every heartbeat says the count: `0 handler calls` is a dead channel,
  `N calls, 0 seen` is a channel that carries something that is not `os_log`.
* `setInvalidationHandler:` is installed, so a refusal now says
  `the stream was invalidated: ...` instead of nothing.
* the first line says `uid ... euid ... gid ...`, so the next log answers the access
  question by itself.
* a console line is written in a loop now; the one write with `O_NONBLOCK` is how
  `streaming, backend ...` was lost from `run.log` in TEST13B while the heartbeat survived.

### The archive, when the live pipe still says nothing

`logd` writes `.tracev3` files into `/private/var/db/diagnostics` (var_store leaves that
directory out of the saved volume on purpose: "tens of MB a boot, and remade"), so the same
`os_log` records are on disk whether or not a live client is served. `oslogfwd` now

* prints `archive ...: N .tracev3, M bytes, newest K s old` 20 s after it starts and every
  five minutes — if that grows, `os_log` itself is alive and only the reader is at fault;
  if it stays empty, the firehose is the fault and no reader can help;
* when the live stream has delivered **nothing** for `oslog_store_secs` (default 120), it
  reads the store from the last boot through `LoggingSupport` (`OSLogEventStore/localStore`
  then a store-to-stream shape, `activateStreamFromLastBoot`) and prints the matching
  entries with the tag `oslog-ar` instead of `oslog`. Timestamps dedup the reads, so the
  repeats every `oslog_store_secs` only print what is new.

Boot-args for that: `oslog_store=0` turns it off, `oslog_store_secs=N` moves the first read,
`oslog_store_wait=N` (default 20) bounds one read. `--store` reads the archive at once and
`--no-store` never does. The read needs root (or `com.apple.private.logging.diagnostic`,
which the entitlements carry); if the store or the shape is missing, the line says which
step failed (`no local store (...)`, `cannot stream the store (...)`).

The daemon starts by itself (`com.hakios.oslogfwd`, `RunAtLoad`); `OSLOGFWD=0` in the build
leaves it out.

## What is not certain yet

The private API's exact creation selector cannot be read out of the cache: the class method
lists in this cache do not decode as ordinary relative method lists, so what is known for
sure is the set of selectors that exist (`initWithLiveSource:`, `setStreamHandler:`,
`setFilterPredicate:`, `activate`, `invalidate`), not which class owns which. `oslogfwd`
therefore tries the documented shapes in order — `+[OSLogEventLiveStore
liveStreamWithFilterPredicate:error:]`, `-[OSLogEventLiveStream initWithLiveSource:]`,
`-[OSLogEventLiveStream initWithSource:]`, `-[OSLogEventLiveStream init]` — and prints the
one that worked in its first line (`backend ...`). A wrong guess is caught, not fatal.

TEST13B settled that part: `initWithLiveSource:` is the shape that reaches
`diagnosticd` (its spawn appears right after the backend line), so the creation and the
activation are right and only the authorisation was missing.

The archive-to-stream shape (which selector of `OSLogEventStore` hands out an
`OSLogEventStream`) is the remaining unknown: the code tries
`streamWithFilterPredicate:error:`, `streamWithPredicate:error:`, `-stream`, then
`-[OSLogEventStream initWithPredicate:]`, and says which one answered. `--selftest` prints
the class/selector matrix and runs one read, so the first phone-side run that is not a full
test settles it in one line.

If none of them can be created on the phone, the line to look for is
`oslogfwd: no live stream (...); retry in N s` (or, for the archive,
`oslogfwd: archive: no local store (...)`), and the next thing to try is the in-process
hook, which needs no stream at all: libsystem_trace exports `_os_log_set_hook` and
`_os_trace_set_mode`, so a library of ours inside the process (the `INJECT` mechanism
already puts one into backboardd, SpringBoard and mediaserverd) can take the messages as
they are written. That is a different design — one process at a time, no daemon — and is not
implemented here.
