#!/usr/bin/env python3
"""A program on the phone's iOS through sweetd (userland/src/sweetd.c: TCP 5143 of en2, the kernel's USB network;
kernel arguments usb_handover=1 usbnet_static=1 and sweetd=1, an image built with SWEETD=1).

    sweet-sh.py [--timeout S] /absolute/program [args...]
    sweet-sh.py ls|cat|sysctl|rm|mkdir|kill|sha256 ARGS...   sweetd's own commands
    sweet-sh.py --put LOCAL REMOTE [MODE]                     a file onto the phone (MODE default 755)

Prints the program's output (stdout and stderr merged) as it comes and exits with its status (128 + signal
when it was killed). The token sweetd wants is the one userland/build.sh put in the image: userland/sweetd.token,
or the file SWEETD_TOKEN names (never in git). The phone is IP (default 10.11.0.2).

From another HakiOS port: telegdyd/ios-vayu, branch vayu-dev, commit 80542801 (its tools/sweet-sh.py, from
ios-jasmine's); here the kernel's USB network's address alone, the token's path from the environment, sha256.
"""
import os
import socket
import sys

HOST = os.environ.get('IP', '10.11.0.2')
PORT = int(os.environ.get('SWEETD_PORT', '5143'))      # (another port: tools/deploy-test's stand-in phone)
END = b'__SWEETD_END__'
TOKEN = os.environ.get('SWEETD_TOKEN') or os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '..',
                                                        'userland', 'sweetd.token')
BUILTINS = ('ls', 'cat', 'sysctl', 'put', 'rm', 'mkdir', 'kill', 'sha256')


def main():
    args = sys.argv[1:]
    timeout = 60
    if args[:1] == ['--timeout']:
        timeout = int(args[1])
        args = args[2:]
    data = b''
    if args[:1] == ['--put'] and len(args) in (3, 4):
        data = open(args[1], 'rb').read()
        args = ['put', args[2], args[3] if len(args) == 4 else '755', str(len(data))]
    if not args or (not args[0].startswith('/') and args[0] not in BUILTINS):
        sys.exit(__doc__)
    try:
        token = open(TOKEN).read().strip()
    except OSError as e:
        sys.exit(f'no sweetd token: {e}')
    try:
        s = socket.create_connection((HOST, PORT), timeout=5)
    except OSError as e:
        sys.exit(f'no sweetd at {HOST}:{PORT}: {e}')
    s.settimeout(timeout + 30)
    s.sendall(f'RUN {token} {timeout} {len(args)}\n'.encode() + b''.join(a.encode() + b'\n' for a in args) + data)
    buf = b''
    while True:
        chunk = s.recv(65536)
        if not chunk:
            break
        buf += chunk
        # print everything that cannot be the start of the end marker
        cut = buf.rfind(b'\n' + END)
        keep = len(buf) - len(END) - 16 if cut < 0 else cut
        if keep > 0:
            sys.stdout.buffer.write(buf[:keep])
            sys.stdout.buffer.flush()
            buf = buf[keep:]
    cut = buf.rfind(b'\n' + END)
    if cut < 0:
        sys.stdout.buffer.write(buf)
        sys.exit('\n(connection closed without a status)')
    sys.stdout.buffer.write(buf[:cut] + b'\n')
    status = int(buf[cut + len(END) + 1:].split()[0])
    sys.exit(status if status >= 0 else 128 - status if status > -1000 else 125)


if __name__ == '__main__':
    main()
