#!/usr/bin/env python3
"""A stand-in for the phone, for tools/deploy-test/run.sh: iOS's command port (ping, reboot) and sweetd (its
protocol and built-ins: cat, sha256, put, rm, ls) on 127.0.0.1, /redwood/bin/flash_stage's show/stage/cancel and
the loader's staged flash (userland/src/flash_stage.c, xnuloader/loader.c standalone_staged_flash) as their effects
on a directory: boot.img (the partition), stage.img and stage.head (the image's bytes and the sha256 of the boot
it was staged against). Nothing of the phone's is touched: this is for the host's half of a deploy.

    fake_phone.py DIR CMD_PORT SWEETD_PORT TOKEN MODE

MODE: ok (the loader writes a stage made against the boot it finds), noflash (the loader writes nothing),
stagefail (flash_stage stage fails), nodown (reboot answers but the phone never restarts); with "-recovery" after
it (ok-recovery, ...) the image now running says flash_stage_target=recovery: flash_stage and the loader stage and
write recovery.img, never boot.img.
"""
import hashlib
import os
import socket
import sys
import threading
import time

D, CMD, SW, TOKEN, MODE = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4], sys.argv[5]
TARGET = 'recovery' if MODE.endswith('-recovery') else 'boot'
MODE = MODE[:-len('-recovery')] if TARGET == 'recovery' else MODE
TIMG = D + '/' + TARGET + '.img'
END = b'__SWEETD_END__'
down_until = 0.0
lock = threading.Lock()


def path(p):
    return {'/dev/rufs_boot': D + '/boot.img', '/dev/rufs_recovery': D + '/recovery.img'}.get(p, D + '/root' + p)


def log(*a):
    with open(D + '/phone.log', 'a') as f:
        print(*a, file=f)


def loader_start():
    """the next start: the loader takes a stage made against this boot, clears any header"""
    head = D + '/stage.head'
    if not os.path.exists(head):
        return
    boot = open(TIMG, 'rb').read()
    against = open(head).read().strip()
    os.unlink(head)
    if MODE == 'noflash' or against != hashlib.sha256(boot).hexdigest():
        log('loader: not flashed')
        return
    img = open(D + '/stage.img', 'rb').read()
    open(TIMG, 'wb').write(img + boot[len(img):])
    log('loader: ' + TARGET + ' written')


def reboot():
    global down_until
    if MODE == 'nodown':
        return
    with lock:
        down_until = time.time() + 4
    loader_start()


def is_down():
    return time.time() < down_until


def recv_line(c):
    b = b''
    while not b.endswith(b'\n'):
        x = c.recv(1)
        if not x:
            break
        b += x
    return b.decode().rstrip('\n')


def cmd_port():
    ls = socket.socket()
    ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    ls.bind(('127.0.0.1', CMD))
    ls.listen(4)
    while True:
        c, _ = ls.accept()
        if is_down():
            c.close()
            continue
        line = recv_line(c)
        if line == 'ping':
            c.sendall(b'pong\n')
        elif line == 'reboot':
            c.sendall(b'reboot: the run ends, the phone restarts\n')
            c.close()
            log('reboot')
            reboot()
            continue
        else:
            c.sendall(b'?\n')
        c.close()


def flash_stage(argv, out):
    head = D + '/stage.head'
    boot_sha = hashlib.sha256(open(TIMG, 'rb').read()).hexdigest()
    t = TARGET.encode()
    if argv[1] == 'show':
        out.append(b'flash_stage: staging area darwin blocks 1..2, header in block 3; target ' + t + b' (/dev/rufs_' + t +
                   b'), 1 bytes, fnv 0\n')
        if os.path.exists(head):
            bound = open(head).read().strip() == boot_sha
            out.append(b'flash_stage: staged: 1 bytes, fnv 0: ' +
                       (b'against this ' + t + b': the loader writes ' + t + b'\n' if bound else b'against another boot\n'))
        else:
            out.append(b'flash_stage: nothing staged\n')
        return 0
    if argv[1] == 'cancel':
        if os.path.exists(head):
            os.unlink(head)
        out.append(b'flash_stage: cancelled: the next start is an ordinary one\n')
        return 0
    if argv[1] == 'stage' and len(argv) == 3:
        if MODE == 'stagefail':
            out.append(b'flash_stage: stage: FAILED; nothing will be flashed\n')
            return 1
        open(D + '/stage.img', 'wb').write(open(path(argv[2]), 'rb').read())
        open(head, 'w').write(boot_sha)
        out.append(b'flash_stage: staged ' + argv[2].encode() + b'\n')
        return 0
    return 2


def serve(c):
    first = recv_line(c).split()
    if len(first) != 4 or first[0] != 'RUN' or first[1] != TOKEN:
        c.sendall(b'sweetd: wrong or missing token\n' + END + b' -1000\n')
        return
    argv = [recv_line(c) for _ in range(int(first[3]))]
    out, status = [], 0
    log('sweetd:', ' '.join(argv))
    if argv[0] == 'cat':
        out.append(open(path(argv[1]), 'rb').read())
    elif argv[0] == 'sha256':
        data = open(path(argv[1]), 'rb').read()
        if len(argv) == 3:
            data = data[:int(argv[2])]
        out.append(f'{hashlib.sha256(data).hexdigest()}  {len(data)} {argv[1]}\n'.encode())
    elif argv[0] == 'put':
        n, data = int(argv[3]), b''
        while len(data) < n:
            data += c.recv(min(65536, n - len(data)))
        os.makedirs(os.path.dirname(path(argv[1])), exist_ok=True)
        open(path(argv[1]), 'wb').write(data)
        out.append(f'put: {argv[1]}: {n} bytes\n'.encode())
    elif argv[0] == 'rm':
        for p in argv[1:]:
            if os.path.exists(path(p)):
                os.unlink(path(p))
    elif argv[0] == '/redwood/bin/flash_stage':
        status = flash_stage(argv, out)
    else:
        out.append(b'sweetd: no such thing here\n')
        status = -1000
    c.sendall(b''.join(out) + b'\n' + END + f' {status}\n'.encode())


def sweetd():
    ls = socket.socket()
    ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    ls.bind(('127.0.0.1', SW))
    ls.listen(4)
    while True:
        c, _ = ls.accept()
        if not is_down():
            try:
                serve(c)
            except OSError:
                pass
        c.close()


threading.Thread(target=cmd_port, daemon=True).start()
sweetd()
