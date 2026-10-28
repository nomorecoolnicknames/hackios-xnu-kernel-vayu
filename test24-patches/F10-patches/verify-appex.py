#!/usr/bin/env python3
"""F10: check the LocalStorage appex plist the way FileProviderDaemon reads it.

-[FPDProviderDescriptor initWithExtension:] (0x1c3c6551c) takes
[bundle infoDictionary][@"NSExtension"][@"NSExtensionAttributes"] and reads
NSExtensionFileProviderDocumentGroup (and the other NSExtensionFileProvider* keys) from it.
The appex in the iOS 16.5 system image has them only at the NSExtension level.
"""
import plistlib
import sys

path = sys.argv[1] if len(sys.argv) > 1 else (
    'devices/poco-x3-pro/userland/share/fp-localstorage/'
    'LocalStorageFileProvider.appex/Info.plist')

with open(path, 'rb') as handle:
    info = plistlib.load(handle)

extension = info.get('NSExtension')
if not isinstance(extension, dict):
    sys.exit('no NSExtension dictionary')

attributes = extension.get('NSExtensionAttributes')
if not isinstance(attributes, dict):
    sys.exit('no NSExtension.NSExtensionAttributes: the daemon descriptor sees no FileProvider keys')

group = attributes.get('NSExtensionFileProviderDocumentGroup')
want = extension.get('NSExtensionFileProviderDocumentGroup')
print('nested keys:', ', '.join(sorted(attributes)))
print('nested document group:', group)
print('top-level document group:', want)
if group != 'group.com.apple.FileProvider.LocalStorage':
    sys.exit('nested document group is %r' % (group,))
if want != group:
    sys.exit('top-level and nested groups differ')
print('OK: the daemon path [NSExtension][NSExtensionAttributes][NSExtensionFileProviderDocumentGroup] is present')
