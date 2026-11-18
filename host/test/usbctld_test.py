#!/usr/bin/env python3
"""usbctld on the host: the real agent, a socket in place of /dev/vayu-ctl.

usbctld is compiled for this machine (it is plain POSIX C: usbctld.c and minish.c), started
with "-d FD" on one end of a socketpair and "-s -" (the built-in shell), and driven by a
Python side that speaks what the kernel speaks (struct vayu_ctl_msg, bsd/sys/vayu_ctl.h):

  kernel -> agent   CMD  (cmd = SH or PULL, arg[], text)
  agent -> kernel   OUT / DONE                              for SH
                    PULLDATA / PULLDONE                     for PULL

What is checked: a command's output and status, the 64 KB cut, the timeout (SIGKILL and status
124), pull in pieces at absolute offsets, pull's end-of-file, a missing file's error, and that
the agent keeps serving after all of it.

usage: python3 -I usbctld_test.py [-v]
"""
import os
import shutil
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time

MAGIC = 0x31474d5343545641
MSG_CMD, MSG_OUT, MSG_DONE, MSG_PULLDATA, MSG_PULLDONE, MSG_HELLO = 1, 2, 3, 4, 5, 6
CMD_TOUCH, CMD_SWIPE, CMD_KEY, CMD_SH, CMD_PULL, CMD_STATUS = 1, 2, 3, 4, 5, 6
MSG = struct.Struct("<QIIQqQ6QII")      # struct vayu_ctl_msg
SH_MAX = 64 * 1024
PULL_MAX = 4 * 1024 * 1024

VERBOSE = "-v" in sys.argv


def note(*a):
    if VERBOSE:
        print("   ", *a)


def find_src():
    here = os.path.dirname(os.path.abspath(__file__))
    candidates = [os.environ.get("USBCTLD_SRC", ""),
                  os.path.join(here, "..", "..", "work", "userland", "src"),
                  os.path.join(here, "..", "..", "userland", "src")]
    for c in candidates:
        if c and os.path.exists(os.path.join(c, "usbctld.c")):
            return os.path.abspath(c)
    raise SystemExit("usbctld_test: usbctld.c not found (set USBCTLD_SRC)")


def build(src, outdir):
    exe = os.path.join(outdir, "usbctld")
    cc = os.environ.get("CC", "cc")
    cmd = [cc, "-O2", "-Wall", "-I", src, "-o", exe,
           os.path.join(src, "usbctld.c"), os.path.join(src, "minish.c")]
    note(" ".join(cmd))
    subprocess.run(cmd, check=True)
    return exe


class Kernel:
    """The other end: what bsd/dev/vayu_ctl.c sends and expects."""

    def __init__(self, exe, timeout=15):
        a, b = socket.socketpair(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock = a
        self.proc = subprocess.Popen([exe, "-d", str(b.fileno()), "-s", "-", "-t", str(timeout)],
                                     pass_fds=(b.fileno(),), close_fds=True)
        b.close()
        hello = self._read_msg()
        assert hello[0] == MSG_HELLO, hello
        self.next_id = 1

    def _read(self, n):
        data = b""
        while len(data) < n:
            part = self.sock.recv(n - len(data))
            if not part:
                raise EOFError("usbctld closed the socket")
            data += part
        return data

    def _read_msg(self):
        head = MSG.unpack(self._read(MSG.size))
        magic, mtype, flags, mid, code, cmd, a0, a1, a2, a3, a4, a5, length, _ = head
        assert magic == MAGIC, hex(magic)
        payload = self._read(length) if length else b""
        return mtype, head, payload

    def _send(self, mtype, mid, code, cmd, args, payload=b""):
        head = MSG.pack(MAGIC, mtype, 0, mid, code, cmd, *args, len(payload), 0)
        self.sock.sendall(head + payload)

    def sh(self, command):
        mid = self.next_id
        self.next_id += 1
        self._send(MSG_CMD, mid, 0, CMD_SH, [0] * 6, command.encode() + b"\0")
        out = b""
        while True:
            mtype, head, payload = self._read_msg()
            if mtype == MSG_OUT:
                out += payload
            elif mtype == MSG_DONE:
                return head[4], out
            else:
                raise AssertionError("unexpected message %d" % mtype)

    def pull(self, path, offset=0, want=PULL_MAX):
        mid = self.next_id
        self.next_id += 1
        self._send(MSG_CMD, mid, 0, CMD_PULL, [offset, want, 0, 0, 0, 0], path.encode() + b"\0")
        data = b""
        while True:
            mtype, head, payload = self._read_msg()
            if mtype == MSG_PULLDATA:
                assert head[5] == offset + len(data), (head[5], offset, len(data))
                data += payload
            elif mtype == MSG_PULLDONE:
                return head[4], head[6], head[6 + 1], data     # code, arg[0]=size, arg[1]=eof
            else:
                raise AssertionError("unexpected message %d" % mtype)

    def close(self):
        try:
            self.sock.close()
        finally:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()


CHECKS = []


def check(fn):
    CHECKS.append(fn)
    return fn


@check
def a_command_and_its_output(k):
    code, out = k.sh("echo hello")
    assert (code, out) == (0, b"hello\n"), (code, out)
    return "echo hello -> %r, status 0" % out


@check
def the_pass_command(k):
    code, out = k.sh("uname -a; ps -A | wc -l")
    text = out.decode("utf-8", "replace")
    assert code == 0, (code, text)
    assert text.splitlines()[0].startswith(("Darwin", "Linux")), text
    assert text.splitlines()[-1].strip() and " " not in text.splitlines()[-1].strip(), text
    return "%r" % text.replace("\n", " | ")


@check
def the_exit_status(k):
    code, out = k.sh("false")
    assert code == 1 and out == b"", (code, out)
    code, out = k.sh("echo no; exit 7")
    assert code == 7 and out == b"no\n", (code, out)
    return "false -> 1, exit 7 -> 7"


@check
def output_of_100k_is_cut_at_64k(k):
    code, out = k.sh("cat /tmp/usbctl-big")
    assert code == 0, code
    assert len(out) == SH_MAX, len(out)
    return "100 KB -> %d bytes (the kernel also sets its truncated flag)" % len(out)


@check
def a_slow_command_is_killed(k):
    began = time.time()
    code, out = k.sh("sleep 10")
    took = time.time() - began
    assert code == 124, (code, out)
    assert took < 6, took
    return "sleep 10 with -t 1... (this shell is -t 1)" if took < 6 else "too slow"


@check
def pull_in_pieces(k):
    with open("/tmp/usbctl-5m", "rb") as f:
        data = f.read()
    code, size, eof, piece = k.pull("/tmp/usbctl-5m", 0, 4 * 1024 * 1024)
    assert code == 0 and size == len(data) and eof == 0 and piece == data[:4 * 1024 * 1024], \
        (code, size, eof, len(piece))
    code, size, eof, piece2 = k.pull("/tmp/usbctl-5m", len(piece), 4 * 1024 * 1024)
    assert code == 0 and eof == 1 and piece + piece2 == data, (code, eof, len(piece2))
    return "5 MB in 4 MB + %d bytes, identical" % len(piece2)


@check
def pull_of_a_missing_file(k):
    code, size, eof, piece = k.pull("/tmp/usbctl-nope", 0, 4096)
    assert code == -1000 - 2 and eof == 1 and piece == b"", (code, size, eof)
    return "ENOENT -> %d" % code


@check
def still_alive_after_all_that(k):
    code, out = k.sh("echo alive")
    assert (code, out) == (0, b"alive\n"), (code, out)
    return "one more command"


def main():
    src = find_src()
    tmp = tempfile.mkdtemp(prefix="usbctld-test-")
    try:
        exe = build(src, tmp)
        with open("/tmp/usbctl-big", "wb") as f:
            f.write(b"0123456789abcdef" * (100 * 1024 // 16))
        with open("/tmp/usbctl-5m", "wb") as f:
            f.write(bytes((i * 13 + 7) & 0xff for i in range(5 * 1024 * 1024)))
        k = Kernel(exe, timeout=1)
        failures = 0
        try:
            for fn in CHECKS:
                try:
                    detail = fn(k)
                except AssertionError as e:
                    failures += 1
                    print("FAIL %-38s %s" % (fn.__name__, e))
                else:
                    print("PASS %-38s %s" % (fn.__name__, detail or ""))
        finally:
            k.close()
        print("%d/%d checks passed" % (len(CHECKS) - failures, len(CHECKS)))
        return 1 if failures else 0
    finally:
        for p in ("/tmp/usbctl-big", "/tmp/usbctl-5m"):
            try:
                os.unlink(p)
            except OSError:
                pass
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
