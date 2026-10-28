# F10 patches — LocalStorage appex `NSExtensionAttributes` and the files domain

Base: TEST22C tree = `wt14` `vayu-test22` + F9 + N9 (the working copy the run used,
`/mnt/ramdisk/t22/ws22c`). The patches apply with `patch -p1` to that tree. They do not
touch the kernel or the system image.

## Why

`-[FPDProviderDescriptor initWithExtension:]` (FileProviderDaemon 16.5, `0x1c3c6551c`)
builds an `NSBundle` from `[NSExtension _extensionBundle].bundleURL`, takes
`[bundle infoDictionary]`, then `[NSExtension][@"NSExtensionAttributes"]`, and reads every
`NSExtensionFileProvider*` key from that **nested** dictionary
(`0x1c3c65c14`–`0x1c3c66038`; the document group itself at `0x1c3c6660c`, cfstring
`0x1d9b7bef0`, on the same `x21` loaded from `[sp,#240]`). The LocalStorage appex in the
iOS 16.5 system image has the keys only directly under `NSExtension`; `NSExtensionAttributes`
is absent, so
`extensionStorageURLs` is empty, `-[FPDExtension initWithExtension:]` logs
`"NSExtensionFileProviderDocumentGroup" key not found in Info.plist of <appex>` /
`The documentStorageURL property on NSFileProviderManager will not be available.`
(`__oslogstring 0x1c3d172fb`, call `0x1c3c76378`) and the provider's storage is nil.

The appex is a plist-only bundle (20 KB, no executable), so the whole fix is a 1.5 KB
`Info.plist` with an added `NSExtensionAttributes` dictionary. `init` bind-mounts the copy
over `/System/Library/Frameworks/FileProvider.framework/PlugIns/LocalStorageFileProvider.appex`
before launchd starts, exactly as it already bind-mounts the audio plug-ins. The system image
does not have to be rebuilt.

## Apply

```sh
cd <tree>
mkdir -p devices/poco-x3-pro/userland/share/fp-localstorage
cp -a F10-patches/appex/LocalStorageFileProvider.appex \
      devices/poco-x3-pro/userland/share/fp-localstorage/

patch -p1 < F10-patches/0001-userland-build-F10-install-LocalStorage-appex.patch
patch -p1 < F10-patches/0002-init-F10-bind-LocalStorage-appex.patch
patch -p1 < F10-patches/0003-fp_enable-F10-read-appex-NSExtensionAttributes.patch
patch -p1 < F10-patches/0004-selftest-rc.jobs-F10-create-the-default-domain.patch
```

Patch 0001 copies the appex into `root/share/fp-localstorage` while the root image is built.
Patch 0002 binds it over the system image's appex in both root modes (`rd=md0` and
`root_apfs=DEV`). Patch 0003 teaches `fp_enable` that the daemon reads the appex's nested
`NSExtensionAttributes`, not the shim's `NSExtension.attributes`, and prints both. Patch
0004 passes `--create` (the daemon's `_recreateDefaultDomainIfNeeded` path) in the boot pass
and in both self-test passes, so the default domain is actually created once the document
group is present.

Build the root image as in TEST22C (`userland/build.sh`, `mkroot.sh`); boot args are unchanged.
Expected new lines: `build.sh: F10: ...`, `[ ok ] the LocalStorage appex with
NSExtension.NSExtensionAttributes`, `record: appex NSExtension.NSExtensionAttributes: 5 key(s)`,
`pkd: ... (from appex NSExtension.NSExtensionAttributes)`, `pkd=yes`, `--create`,
`storage=yes` and `files: PASS`.

## Payload

`appex/LocalStorageFileProvider.appex/Info.plist` is the original iOS 16.5 plist plus:

```xml
<key>NSExtensionAttributes</key>
<dict>
    <key>NSExtensionFileProviderDocumentGroup</key><string>group.com.apple.FileProvider.LocalStorage</string>
    <key>NSExtensionFileProviderEnabledByDefault</key><true/>
    <key>NSExtensionFileProviderSupportsEnumeration</key><true/>
    <key>NSFileProviderDefaultDomainEnabled</key><true/>
    <key>NSFileProviderDecorations</key> ... (same array as the top level)
</dict>
```

The top-level keys are kept for LaunchServices, `ls_rebuild`, and the existing `fp_enable`
`storage:` probe.

`verify-appex.py` checks the nested key without a device.
