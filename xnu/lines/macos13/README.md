# The macOS 13.4 kernel line (xnu-8796.121.2)

The kernel this port runs macOS 13.4 with: Apple's xnu-8796.121.2 (the same release as the iOS 16.5 kernel in `xnu/`), the
iOS 16.5 line's drivers, and what macOS needs on top of them (its storage nodes, the HID and pointer services, an
EndpointSecurity stand-in, the GPU for HakiosMetal, the NBD disks its volumes come from).

- `hakios.patch`: our changes to Apple's files; `files/`: the new files; `hfs.patch`: our changes to Apple's HFS Plus
  (hfs-627.100.6, fetched as `xnu/` fetches it).
- `xnu/line.sh apply macos13 [DIR]` fetches Apple's tag and lays this over it (default `xnu-src/macos13`);
  `xnu/line.sh export macos13 DIR` is the reverse. It builds as the iOS 16.5 kernel does (`./xnu-build.sh`, `xnu/README.md`).

Everything new is behind a boot argument and off by default; macOS's own behaviour needs `macos_abi=1`.

## State (runs on the phone, 20-21 November 2026)

- macOS 13.4 boots on the phone with its system and data volumes on NBD disks: WindowServer, loginwindow, the Dock and
  the menu bar run, and the Adreno 640 draws for WindowServer through HakiosMetal.
- The desktop comes up on the panel (portrait, HiDPI 2x): the menu bar, the Dock and windows. The wallpaper's window is
  on screen but its picture is not drawn yet.
- Touch and the cursor do not work yet.
- `vayu_fbthumb=N` (as in the iOS 27 line) and `tools/fbthumb2png.py` give pictures of the panel from the kernel's log.
