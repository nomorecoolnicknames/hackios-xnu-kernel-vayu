# The iOS 27 kernel line (xnu-12377)

The kernel this port runs iOS 27 with: Scion's iOS 27 kernel ([Scion-Project/xnu](https://github.com/Scion-Project/xnu),
commit `c2f63b2`: xnu-12377.121.6 with Scion's iOS 27 layer, APSL 2.0) and this phone's board and drivers on top, ported
from the iOS 16.5 line (`xnu/`). Apple has not published xnu-12377.121.6 (its newest xnu-12377 tag is 12377.81.4), so the
base is Scion's public tree at the commit this work started from.

- `hakios.patch`: our changes to the base's files; `files/`: the new files (the VAYU board, `bsd/dev/vayu_*.c`,
  `PORTING.md`, `tools/vayu/`).
- `xnu/line.sh apply ios27 [DIR]` fetches the base by its commit and lays this over it (default `xnu-src/ios27`);
  `xnu/line.sh export ios27 DIR` is the reverse.
- Build: on a Mac with Xcode and the KDK, `tools/vayu/build-mac.sh` (see `files/PORTING.md`); the kernel is
  `BUILD/obj/DEVELOPMENT_ARM64_VAYU/mach.development.vayu`. It needs the boot loader's xnu-12377 mode (`xnu=12377`).

Everything that touches the phone's hardware is behind a boot argument and off by default (the head of each driver lists
its own).

## State (runs on the phone, 21 November 2026)

- iOS 27's user space starts from a root on the computer at the other end of the USB cable (NBD, `nbd=1`, then
  `kern.hakios_switch_root`): launchd and about a hundred processes.
- Most daemons used to kill themselves a few seconds in: the kernel read iOS 27's `posix_spawn` attributes in xnu-12377's
  older layout, so launchd's restart timeout became a kqworkloop limit. `vayu_spawnattr=27` reads them where iOS 27 puts
  them; with it that stopped.
- SpringBoard currently crash-loops on SIGABRT. There is no picture of the system on the panel yet: the boot console and
  the logo, then a black screen.
- `vayu_fbthumb=N` puts a thumbnail of what the panel shows into the kernel's log every N seconds; `tools/fbthumb2png.py`
  turns a log back into pictures.
