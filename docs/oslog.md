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

If none of them can be created on the phone, the line to look for is
`oslogfwd: no live stream (...); retry in N s`, and the next thing to try is the in-process
hook, which needs no stream at all: libsystem_trace exports `_os_log_set_hook` and
`_os_trace_set_mode`, so a library of ours inside the process (the `INJECT` mechanism
already puts one into backboardd, SpringBoard and mediaserverd) can take the messages as
they are written. That is a different design — one process at a time, no daemon — and is not
implemented here.
