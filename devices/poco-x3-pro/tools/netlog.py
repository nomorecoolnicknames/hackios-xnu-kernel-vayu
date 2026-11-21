#!/usr/bin/env python3
"""The kernel's log, live, over the kernel's USB network (bsd/dev/vayu_usbnet_ecm.c, kernel argument usbnet_log=1:
TCP 5140 of en2).

The phone sends its log ring -- the first 512 KB of the boot, then the newest -- as the loader would get it back
after a run, then new lines as they come. This connects (retrying until the phone is up), copies everything to
stdout and appends it to OUT, and when the connection ends (a restart) connects again while --wait allows.

    netlog.py OUT [--wait SECONDS] [--for SECONDS] [--once]

--wait: give up if no connection within this time (default 300); again after each connection that ends.
--for:  stop this long after the first connection (default: never; ^C).
--once: stop when the first connection ends.

From another HakiOS port: telegdyd/ios-vayu, branch vayu-dev, commit 66d2e6bcb3c0 (its tools/sweet-netlog.py, from
ios-jasmine); here the one address, reconnects, and a mark in OUT where each connection begins.
"""
import argparse
import os
import socket
import sys
import time

HOST = os.environ.get('IP', '10.11.0.2')
PORT = 5140


def connect(deadline):
    while time.time() < deadline:
        try:
            return socket.create_connection((HOST, PORT), timeout=2)
        except OSError:
            time.sleep(1)
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('out')
    ap.add_argument('--wait', type=float, default=300)
    ap.add_argument('--for', dest='duration', type=float, default=0)
    ap.add_argument('--once', action='store_true')
    a = ap.parse_args()
    end = None
    with open(a.out, 'ab') as f:
        while True:
            sock = connect(time.time() + a.wait if end is None else min(end, time.time() + a.wait))
            if sock is None:
                print(f'# no log at {HOST}:{PORT} (usbnet_log=1?)', file=sys.stderr)
                return 1
            if end is None and a.duration:
                end = time.time() + a.duration
            mark = f'\n# netlog: connected to {HOST}:{PORT} at {time.strftime("%F %T")}\n'.encode()
            f.write(mark)
            print(mark.decode().strip(), file=sys.stderr)
            sock.settimeout(1)
            while end is None or time.time() < end:
                try:
                    data = sock.recv(65536)
                except socket.timeout:
                    continue
                except OSError:
                    break
                if not data:
                    break
                f.write(data)
                f.flush()
                sys.stdout.buffer.write(data)
                sys.stdout.buffer.flush()
            sock.close()
            print('# netlog: connection closed', file=sys.stderr)
            if a.once or (end is not None and time.time() >= end):
                return 0


if __name__ == '__main__':
    sys.exit(main())
