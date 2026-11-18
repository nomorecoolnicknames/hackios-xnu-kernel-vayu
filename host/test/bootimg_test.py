#!/usr/bin/env python3
"""
Host test for host/new/boot-usbctl.img: the boot image must be the same recipe
as host/boot-test4.img, with only loader.efi replaced.

Checks, without a phone:
  1. both are Android boot images, header v1, page 2048, same addresses;
  2. the XNUP payload of the base has loader.efi (2.1), usb=1, platform=vayu;
  3. repacking the base from its own pieces reproduces it byte for byte
     (the same test for the new image shows the recipe is reproducible);
  4. the new payload has loader.efi 2.3 with oem touch/swipe/key/sh/pull and
     the same usb/platform entries;
  5. BootShim, UEFI FD, DTB and ramdisk are identical in both images;
  6. the header id is sha1(kernel|size|ramdisk|size|second|size|dtbo|size).
"""
import gzip, hashlib, os, shutil, struct, subprocess, sys, tempfile, zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
BASE = os.path.join(ROOT, 'host', 'boot-test4.img')
NEW = os.path.join(ROOT, 'host', 'new', 'boot-usbctl.img')
MKPAYLOAD = os.path.join(ROOT, 'host', 'mkpayload.py')
MKBOOTIMG = os.path.join(ROOT, 'host', 'new', 'mkbootimg.py')
OLD_LOADER = os.path.join(ROOT, 'work', 'boot', 'loader-2.1.efi')
NEW_LOADER = os.path.join(ROOT, 'artifacts', 'loader-2.3-usbctl.efi')

passed = failed = 0


def check(name, ok, detail=''):
    global passed, failed
    if ok:
        passed += 1
        print(f'PASS {name}' + (f': {detail}' if detail else ''))
    else:
        failed += 1
        print(f'FAIL {name}' + (f': {detail}' if detail else ''))


def parse(path):
    d = open(path, 'rb').read()
    magic, ks, ka, rs, ra, ss, sa, ta, ps, hv, ov = struct.unpack_from('<8s10I', d, 0)
    assert magic == b'ANDROID!' and hv == 1, (magic, hv)
    kernel = d[ps:ps + ks]
    z = zlib.decompressobj(16 + zlib.MAX_WBITS)
    blob = z.decompress(kernel)
    dtb = z.unused_data
    fv = blob.find(b'_FVH') - 0x28
    assert fv > 0 and blob[fv + 0x28:fv + 0x2c] == b'_FVH'
    fvlen, = struct.unpack_from('<I', blob, fv + 0x20)
    shim, fd, xnup = blob[:fv], blob[fv:fv + fvlen], blob[fv + fvlen:]
    assert xnup[:4] == b'XNUP'
    size, = struct.unpack_from('<I', xnup, 4)
    p, entries = 8, {}
    while True:
        name, n, _ = struct.unpack_from('<16sII', xnup, p)
        if not name.strip(b'\0'):
            break
        entries[name.rstrip(b'\0').decode()] = xnup[p + 24:p + 24 + n]
        p += 24 + n + (-n % 16)
    roff = ps + ((ks + ps - 1) // ps) * ps
    ramdisk = d[roff:roff + rs]
    return dict(data=d, ks=ks, ka=ka, rs=rs, ra=ra, ss=ss, sa=sa, ta=ta, ps=ps,
                kernel=kernel, blob=blob, dtb=dtb, shim=shim, fd=fd, xnup=xnup,
                entries=entries, ramdisk=ramdisk)


def repack(base, loader, out):
    tmp = tempfile.mkdtemp(prefix='bootimg-test-')
    payload = os.path.join(tmp, 'payload.bin')
    kernel = os.path.join(tmp, 'kernel.bin')
    args = [sys.executable, '-I', MKPAYLOAD, '-o', payload, '--platform', 'vayu']
    if base['entries'].get('usb', b'')[:1] == b'1':
        args.append('--usb')
    args.append('loader.efi=' + loader)
    subprocess.check_call(args, stdout=subprocess.DEVNULL)
    blob = base['shim'] + base['fd'] + open(payload, 'rb').read()
    blob += b'\0' * (-len(blob) % 16)
    open(kernel, 'wb').write(gzip.compress(blob, 6, mtime=0) + base['dtb'])
    # mkbootimg needs the ramdisk file: write it next to the kernel first
    open(os.path.join(tmp, 'ramdisk.bin'), 'wb').write(base['ramdisk'])
    subprocess.check_call([sys.executable, '-I', MKBOOTIMG, '--kernel', kernel,
                           '--ramdisk', os.path.join(tmp, 'ramdisk.bin'),
                           '--header_version', '1', '-o', out],
                          stdout=subprocess.DEVNULL)


def main():
    b, n = parse(BASE), parse(NEW)
    check('base header v1, page 2048', b['ps'] == 2048 and b['ks'] > 0,
          f"kernel {b['ks']}, page {b['ps']}")
    check('new header v1, page 2048', n['ps'] == 2048 and n['ks'] > 0,
          f"kernel {n['ks']}, page {n['ps']}, file {len(n['data'])}")
    check('load addresses identical',
          (b['ka'], b['ra'], b['sa'], b['ta']) == (n['ka'], n['ra'], n['sa'], n['ta']),
          f"{hex(n['ka'])}, {hex(n['ra'])}, {hex(n['sa'])}, {hex(n['ta'])}")
    check('shim + UEFI FD + DTB + ramdisk identical',
          b['shim'] == n['shim'] and b['fd'] == n['fd'] and b['dtb'] == n['dtb']
          and b['ramdisk'] == n['ramdisk'],
          f"shim {len(n['shim'])}, fd {len(n['fd'])}, dtb {len(n['dtb'])}, "
          f"ramdisk {len(n['ramdisk'])}")
    check('base payload entries', b['entries'].get('usb') == b'1\0'
          and b['entries'].get('platform') == b'vayu' and len(b['entries']['loader.efi']) == 102912,
          'loader.efi 102912, usb=1, platform=vayu')
    check('new payload entries', n['entries'].get('usb') == b'1\0'
          and n['entries'].get('platform') == b'vayu' and len(n['entries']['loader.efi']) == 107008,
          'loader.efi 107008, usb=1, platform=vayu')
    check('base loader reports 2.1', b'2.1\0' in b['entries']['loader.efi']
          and b'oem touch ' not in b['entries']['loader.efi'],
          'no oem touch')
    for label, s in (('2.3', b'2.3\0'), ('oem touch', b'oem touch '),
                     ('oem swipe', b'oem swipe '), ('oem key', b'oem key '),
                     ('oem sh', b'oem sh '), ('oem pull', b'oem pull '),
                     ('oem screenshot', b'oem screenshot')):
        check('new loader has ' + label, s in n['entries']['loader.efi'])
    check('new loader is not the old one',
          n['entries']['loader.efi'] != b['entries']['loader.efi'])
    check('new loader is the 2.1-based build: no 2.2 in-run receive code',
          b'usb: the receive waited' not in n['entries']['loader.efi']
          and b'the stack is restarted while the kernel runs' not in n['entries']['loader.efi'],
          '2.2 receive re-arm/restart absent')
    artifact = open(NEW_LOADER, 'rb').read()
    check('new payload loader is artifacts/loader-2.3-usbctl.efi',
          artifact == n['entries']['loader.efi'],
          f'{len(artifact)} bytes, sha256 {hashlib.sha256(artifact).hexdigest()[:16]}')
    srcdir = os.path.join(ROOT, 'work', 'loader23min')
    if shutil.which('clang') and shutil.which('lld-link'):
        tmp = tempfile.mkdtemp(prefix='bootimg-test-')
        for f in ('loader.c', 'build.sh', 'identity.h', 'efi.h'):
            shutil.copy2(os.path.join(srcdir, f), os.path.join(tmp, f))
        os.chmod(os.path.join(tmp, 'build.sh'), 0o755)
        subprocess.check_call(['./build.sh'], cwd=tmp,
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        rebuilt = open(os.path.join(tmp, 'loader.efi'), 'rb').read()
        src = open(os.path.join(tmp, 'loader.c')).read()
        check('loader source rebuilds to the artifact (SILENCE_SECONDS 50)',
              rebuilt == artifact and '#define SILENCE_SECONDS         50' in src,
              f'{len(rebuilt)} bytes, sha256 {hashlib.sha256(rebuilt).hexdigest()[:16]}')
    else:
        print('SKIP loader rebuild: clang or lld-link is not here')

    if os.path.isfile(OLD_LOADER):
        tmp = tempfile.mkdtemp(prefix='bootimg-test-')
        out = os.path.join(tmp, 'old.img')
        repack(b, OLD_LOADER, out)
        check('repacking the base gives it byte for byte',
              open(out, 'rb').read() == b['data'], hashlib.sha256(open(out, 'rb').read()).hexdigest())
    else:
        print(f'SKIP repacking the base: {OLD_LOADER} not extracted here')

    tmp = tempfile.mkdtemp(prefix='bootimg-test-')
    out = os.path.join(tmp, 'new.img')
    repack(b, os.path.join(ROOT, 'artifacts', 'loader-2.3-usbctl.efi'), out)
    check('repacking the new image is reproducible',
          open(out, 'rb').read() == n['data'], hashlib.sha256(open(out, 'rb').read()).hexdigest())

    # header id: update_sha() appends each field's size as u32
    sha = hashlib.sha1()
    for data in (n['kernel'], n['ramdisk'], b''):
        sha.update(data)
        sha.update(struct.pack('I', len(data)))
    sha.update(b'')
    sha.update(struct.pack('I', 0))
    check('header id', n['data'][576:596] == sha.digest(), sha.hexdigest()[:16])

    print(f'{passed}/{passed + failed}')
    return 0 if failed == 0 else 1


if __name__ == '__main__':
    sys.exit(main())
