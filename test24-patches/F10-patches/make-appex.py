#!/usr/bin/env python3
"""F10: the LocalStorage appex with NSExtension.NSExtensionAttributes, made from your own system image.

usage: make-appex.py SRC.appex DST.appex

SRC is the appex of the iOS 16.5 system image you unpacked yourself (sysimage/unpack.sh):
System/Library/Frameworks/FileProvider.framework/PlugIns/LocalStorageFileProvider.appex.
DST gets the bundle's three plists (Info.plist, InfoPlist.loctable, _CodeSignature/CodeResources: the
plist-only copy F10 was run with) and its Info.plist an NSExtensionAttributes dictionary with the
NSExtensionFileProvider* keys of the NSExtension level, which is where
-[FPDProviderDescriptor initWithExtension:] (0x1c3c6551c) reads them. The top-level keys stay.
Nothing of Apple's is in this repository: the copy and the change are made on your machine.
"""
import os
import plistlib
import shutil
import sys

FILES = ("Info.plist", "InfoPlist.loctable", "_CodeSignature/CodeResources")
KEYS = ("NSExtensionFileProviderDocumentGroup", "NSExtensionFileProviderEnabledByDefault",
        "NSExtensionFileProviderSupportsEnumeration", "NSFileProviderDefaultDomainEnabled",
        "NSFileProviderDecorations")
GROUP = "group.com.apple.FileProvider.LocalStorage"


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__.strip().splitlines()[2])
    src, dst = sys.argv[1], sys.argv[2]
    if not os.path.isfile(os.path.join(src, "Info.plist")):
        sys.exit("make-appex.py: %s has no Info.plist" % src)
    if os.path.exists(dst):
        sys.exit("make-appex.py: %s exists" % dst)
    for name in FILES:
        os.makedirs(os.path.dirname(os.path.join(dst, name)), exist_ok=True)
        shutil.copyfile(os.path.join(src, name), os.path.join(dst, name))
    path = os.path.join(dst, "Info.plist")
    with open(path, "rb") as handle:
        info = plistlib.load(handle)
    extension = info.get("NSExtension")
    if not isinstance(extension, dict) or extension.get("NSExtensionFileProviderDocumentGroup") != GROUP:
        sys.exit("make-appex.py: %s is not the LocalStorage appex of iOS 16.5" % src)
    extension["NSExtensionAttributes"] = {k: extension[k] for k in KEYS if k in extension}
    with open(path, "wb") as handle:
        plistlib.dump(info, handle, fmt=plistlib.FMT_XML, sort_keys=False)
    print("make-appex.py: %s: NSExtension.NSExtensionAttributes with %d key(s)"
          % (dst, len(extension["NSExtensionAttributes"])))


if __name__ == "__main__":
    main()
