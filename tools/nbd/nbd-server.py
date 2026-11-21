#!/usr/bin/env python3
"""
nbd-server.py [options] NAME=IMAGE [NAME=IMAGE ...]: disk images of this computer for the phone's /dev/nbd0
(the kernel's bsd/dev/vayu_nbd.c, boot-args nbd=1 nbd_host=10.11.0.1 nbd_export=NAME), over the USB network
(the phone 10.11.0.2, this end 10.11.0.1: vayu_usbnet=3).

The standard NBD protocol (https://github.com/NetworkBlockDevice/nbd/blob/master/doc/proto.md), the fixed
newstyle handshake (NBD_OPT_GO, NBD_OPT_EXPORT_NAME, NBD_OPT_INFO, NBD_OPT_LIST, NBD_OPT_ABORT), simple
replies; Linux's nbd-client and qemu-nbd's client speak it too, so an image can be checked from a Linux box
before the phone gets it:  nbd-client -N NAME 10.11.0.1 10809 /dev/nbd0 -readonly
Only Python's standard library: for a host that has no nbd-server.

READ ONLY by default: a write is answered EPERM and the export says NBD_FLAG_READ_ONLY. Two ways to write:
  --cow DIR  the images stay as they are; what the phone writes goes to DIR/NAME.cow (sparse, the image's
             size) and DIR/NAME.cow.map (one bit per 4 KiB block: written or not). Reads take written blocks
             from there. Delete both to go back to the image; the image itself is never opened for writing.
  --rw       the images themselves are written. Only for an image that is a copy.
A flush is an fsync (of the overlay and its map with --cow).

  --listen ADDR   the address to take connections on (default 10.11.0.1; 127.0.0.1 for QEMU's user network,
                  whose host 10.11.0.1 is this machine's loopback)
  --port N        default 10809
  --once          stop after the first client has gone (tests)
  --workers N     reads of one connection served by N threads at once, each reply sent when its read is done --
                  out of order, as NBD allows (default 1: one request after the other, replies in order). For the
                  kernel's vayu_nbd_mq=N (several requests in flight, replies matched by cookie). A write or a
                  flush waits until every read before it has been answered, and is done in order.
  --crc MB        say the CRC-32 of each export's first MB MiB before serving: what the kernel's
                  nbd_selftest=MB must say ("nbd: selftest: ... crc32 XXXXXXXX")
The first export is also the default one (an empty name). Every connection gets a line when it comes and
when it goes, with what it read and wrote.
"""
import argparse
import os
import socket
import struct
import sys
import threading
import time
import zlib

NBD_MAGIC = 0x4E42444D41474943          # "NBDMAGIC"
IHAVEOPT = 0x49484156454F5054           # "IHAVEOPT"
REP_MAGIC = 0x0003E889045565A9
REQUEST_MAGIC = 0x25609513
REPLY_MAGIC = 0x67446698

FLAG_FIXED_NEWSTYLE, FLAG_NO_ZEROES = 1, 2
OPT_EXPORT_NAME, OPT_ABORT, OPT_LIST, OPT_INFO, OPT_GO = 1, 2, 3, 6, 7
REP_ACK, REP_SERVER, REP_INFO = 1, 2, 3
REP_ERR_UNSUP, REP_ERR_POLICY, REP_ERR_INVALID, REP_ERR_UNKNOWN = 0x80000001, 0x80000002, 0x80000003, 0x80000006
INFO_EXPORT = 0
TF_HAS_FLAGS, TF_READ_ONLY, TF_SEND_FLUSH, TF_SEND_FUA = 1, 2, 4, 8
CMD_READ, CMD_WRITE, CMD_DISC, CMD_FLUSH = 0, 1, 2, 3
CMD_FLAG_FUA = 1
EPERM, EIO, EINVAL, ENOSPC = 1, 5, 22, 28
MAX_REQUEST = 32 << 20
COW_BLOCK = 4096


def recv_exact(sock, n):
    buf = bytearray(n)
    view = memoryview(buf)
    got = 0
    while got < n:
        k = sock.recv_into(view[got:], n - got)
        if k == 0:
            raise ConnectionError("the client went away")
        got += k
    return bytes(buf)


class Image:
    """An image, read only, written in place (--rw), or with its writes kept aside (--cow)."""

    def __init__(self, name, path, mode, cow_dir):
        self.name, self.path, self.mode = name, path, mode
        self.fd = os.open(path, os.O_RDWR if mode == "rw" else os.O_RDONLY)
        self.size = os.lseek(self.fd, 0, os.SEEK_END)
        self.lock = threading.Lock()
        self.cow = self.map = None
        if mode == "cow":
            base = os.path.join(cow_dir, name or "default")
            self.cow_path, self.map_path = base + ".cow", base + ".cow.map"
            self.cow = os.open(self.cow_path, os.O_RDWR | os.O_CREAT, 0o644)
            if os.fstat(self.cow).st_size != self.size:
                os.ftruncate(self.cow, self.size)
            blocks = (self.size + COW_BLOCK - 1) // COW_BLOCK
            self.map = bytearray((blocks + 7) // 8)
            if os.path.exists(self.map_path):
                with open(self.map_path, "rb") as f:
                    old = f.read()
                self.map[:len(old)] = old[:len(self.map)]
            self.map_dirty = False

    def writable(self):
        return self.mode in ("rw", "cow")

    def _written(self, block):
        return self.map[block >> 3] & (1 << (block & 7))

    def read(self, offset, length):
        if self.mode != "cow":
            return os.pread(self.fd, length, offset)
        out = bytearray()
        end = offset + length
        with self.lock:
            while offset < end:
                block = offset // COW_BLOCK
                src = self.cow if self._written(block) else self.fd
                # a run of blocks from the same place, in one read
                run_end = min(end, (block + 1) * COW_BLOCK)
                while run_end < end and (self._written(run_end // COW_BLOCK) != 0) == (src == self.cow):
                    run_end = min(end, run_end + COW_BLOCK)
                piece = os.pread(src, run_end - offset, offset)
                out += piece.ljust(run_end - offset, b"\0")
                offset = run_end
        return bytes(out)

    def write(self, offset, data):
        if self.mode == "rw":
            os.pwrite(self.fd, data, offset)
            return
        with self.lock:
            end = offset + len(data)
            first, last = offset // COW_BLOCK, (end - 1) // COW_BLOCK
            # the blocks only partly written, copied from the image first if they are not in the overlay yet
            for block in {first, last}:
                if not self._written(block) and (offset > block * COW_BLOCK or end < (block + 1) * COW_BLOCK):
                    os.pwrite(self.cow, os.pread(self.fd, COW_BLOCK, block * COW_BLOCK), block * COW_BLOCK)
            os.pwrite(self.cow, data, offset)
            for block in range(first, last + 1):
                self.map[block >> 3] |= 1 << (block & 7)
            self.map_dirty = True

    def flush(self):
        if self.mode == "rw":
            os.fsync(self.fd)
        elif self.mode == "cow":
            with self.lock:
                os.fsync(self.cow)
                if self.map_dirty:
                    tmp = self.map_path + ".new"
                    with open(tmp, "wb") as f:
                        f.write(self.map)
                        f.flush()
                        os.fsync(f.fileno())
                    os.replace(tmp, self.map_path)
                    self.map_dirty = False


def log(text):
    print(time.strftime("%H:%M:%S"), text, flush=True)


class Client:
    def __init__(self, sock, peer, exports, workers=1):
        self.sock, self.peer, self.exports = sock, peer, exports
        self.no_zeroes = False
        self.read_bytes = self.written_bytes = self.requests = 0
        self.workers = max(1, workers)
        self.send_lock = threading.Lock()
        self.busy = threading.Condition()       # reads given to the workers and not answered yet
        self.pending = 0
        self.most_pending = 0
        self.jobs = None

    def send(self, data):
        with self.send_lock:
            self.sock.sendall(data)

    def _worker(self, image):
        while True:
            job = self.jobs.get()
            if job is None:
                return
            cookie, offset, length = job
            try:
                data = image.read(offset, length)
                reply = struct.pack(">IIQ", REPLY_MAGIC, 0, cookie) + data
                with self.busy:
                    self.read_bytes += length
            except OSError:
                reply = struct.pack(">IIQ", REPLY_MAGIC, EIO, cookie)
            try:
                self.send(reply)
            except OSError:
                pass                            # (the connection went: serve() finds it so)
            with self.busy:
                self.pending -= 1
                self.busy.notify_all()

    def _drain(self):
        """every read handed to the workers answered (before a write, a flush, the end)"""
        if self.jobs is not None:
            with self.busy:
                while self.pending:
                    self.busy.wait()

    def reply(self, option, kind, data=b""):
        self.send(struct.pack(">QIII", REP_MAGIC, option, kind, len(data)) + data)

    def export_of(self, name):
        if name == "" and self.exports:
            return next(iter(self.exports.values()))
        return self.exports.get(name)

    @staticmethod
    def tflags(image):
        flags = TF_HAS_FLAGS | TF_SEND_FLUSH | TF_SEND_FUA
        return flags if image.writable() else flags | TF_READ_ONLY

    def handshake(self):
        """The image the client chose, or None (it went away, or asked for nothing that is here)."""
        self.send(struct.pack(">QQH", NBD_MAGIC, IHAVEOPT, FLAG_FIXED_NEWSTYLE | FLAG_NO_ZEROES))
        (cflags,) = struct.unpack(">I", recv_exact(self.sock, 4))
        if not cflags & FLAG_FIXED_NEWSTYLE:
            log(f"{self.peer}: not a fixed-newstyle client (flags {cflags:#x})")
            return None
        self.no_zeroes = bool(cflags & FLAG_NO_ZEROES)
        while True:
            magic, option, length = struct.unpack(">QII", recv_exact(self.sock, 16))
            if magic != IHAVEOPT or length > 65536:
                log(f"{self.peer}: not an option ({magic:#x}, {length} bytes)")
                return None
            data = recv_exact(self.sock, length)
            if option == OPT_EXPORT_NAME:
                image = self.export_of(data.decode("utf-8", "replace"))
                if image is None:
                    log(f"{self.peer}: no export {data!r}")
                    return None             # the protocol's only answer: hang up
                self.send(struct.pack(">QH", image.size, self.tflags(image)) + (b"" if self.no_zeroes else b"\0" * 124))
                return image
            if option in (OPT_GO, OPT_INFO):
                if length < 6:
                    self.reply(option, REP_ERR_INVALID, b"short GO")
                    continue
                (name_length,) = struct.unpack(">I", data[:4])
                name = data[4:4 + name_length].decode("utf-8", "replace")
                image = self.export_of(name)
                if image is None:
                    self.reply(option, REP_ERR_UNKNOWN, f"no export named {name!r}".encode())
                    continue
                self.reply(option, REP_INFO, struct.pack(">HQH", INFO_EXPORT, image.size, self.tflags(image)))
                self.reply(option, REP_ACK)
                if option == OPT_GO:
                    return image
                continue
            if option == OPT_LIST:
                for name in self.exports:
                    encoded = name.encode()
                    self.reply(option, REP_SERVER, struct.pack(">I", len(encoded)) + encoded)
                self.reply(option, REP_ACK)
                continue
            if option == OPT_ABORT:
                self.reply(option, REP_ACK)
                return None
            self.reply(option, REP_ERR_UNSUP)

    def serve(self, image):
        if self.workers > 1:
            import queue
            self.jobs = queue.Queue()
            for _ in range(self.workers):
                threading.Thread(target=self._worker, args=(image,), daemon=True).start()
        try:
            self._serve(image)
        finally:
            if self.jobs is not None:
                self._drain()
                for _ in range(self.workers):
                    self.jobs.put(None)

    def _serve(self, image):
        while True:
            magic, flags, kind, cookie, offset, length = struct.unpack(">IHHQQI", recv_exact(self.sock, 28))
            if magic != REQUEST_MAGIC:
                log(f"{self.peer}: not a request ({magic:#x}): hung up")
                return
            self.requests += 1
            head = lambda error: struct.pack(">IIQ", REPLY_MAGIC, error, cookie)
            if kind == CMD_DISC:
                return
            if kind != CMD_READ:
                self._drain()                   # (in order after every read before it)
            if kind == CMD_READ:
                if length > MAX_REQUEST or offset + length > image.size:
                    self.send(head(EINVAL))
                    continue
                if self.jobs is not None:
                    with self.busy:
                        self.pending += 1
                        self.most_pending = max(self.most_pending, self.pending)
                    self.jobs.put((cookie, offset, length))
                    continue
                try:
                    data = image.read(offset, length)
                except OSError:
                    self.send(head(EIO))
                    continue
                self.send(head(0) + data)
                self.read_bytes += length
            elif kind == CMD_WRITE:
                if length > MAX_REQUEST:
                    log(f"{self.peer}: a write of {length} bytes: hung up")
                    return
                data = recv_exact(self.sock, length)
                if not image.writable():
                    self.send(head(EPERM))
                elif offset + length > image.size:
                    self.send(head(ENOSPC))
                else:
                    try:
                        image.write(offset, data)
                        if flags & CMD_FLAG_FUA:
                            image.flush()
                        self.send(head(0))
                        self.written_bytes += length
                    except OSError:
                        self.send(head(EIO))
            elif kind == CMD_FLUSH:
                try:
                    image.flush()
                    self.send(head(0))
                except OSError:
                    self.send(head(EIO))
            else:
                self.send(head(EINVAL))

    def run(self):
        started = time.monotonic()
        image = None
        try:
            self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            image = self.handshake()
            if image is not None:
                log(f"{self.peer}: export {image.name!r} ({image.path}, {image.size} bytes, {image.mode})")
                self.serve(image)
        except (ConnectionError, OSError, struct.error) as e:
            log(f"{self.peer}: {e}")
        finally:
            if image is not None and image.writable():
                try:
                    image.flush()
                except OSError as e:
                    log(f"{self.peer}: flush at the end: {e}")
            self.sock.close()
            secs = time.monotonic() - started
            log(f"{self.peer}: gone after {secs:.1f} s: {self.requests} requests, {self.read_bytes >> 20} MB read"
                f" ({self.read_bytes / max(secs, 1e-6) / 1e6:.1f} MB/s), {self.written_bytes >> 10} KB written"
                + (f", at most {self.most_pending} reads at once ({self.workers} workers)" if self.workers > 1 else ""))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--listen", default="10.11.0.1")
    ap.add_argument("--port", type=int, default=10809)
    ap.add_argument("--rw", action="store_true")
    ap.add_argument("--cow", metavar="DIR")
    ap.add_argument("--once", action="store_true")
    ap.add_argument("--workers", type=int, default=1, metavar="N")
    ap.add_argument("--crc", type=int, metavar="MB")
    ap.add_argument("exports", nargs="+", metavar="NAME=IMAGE")
    args = ap.parse_args()
    if args.rw and args.cow:
        ap.error("--rw or --cow, not both")
    mode = "rw" if args.rw else "cow" if args.cow else "ro"
    if args.cow:
        os.makedirs(args.cow, exist_ok=True)
    exports = {}
    for spec in args.exports:
        name, sep, path = spec.partition("=")
        if not sep or not path:
            ap.error(f"{spec}: NAME=IMAGE")
        exports[name] = Image(name, path, mode, args.cow)
        log(f"export {name!r}: {path}, {exports[name].size} bytes, {mode}")
        if args.crc:
            image, crc, done = exports[name], 0, 0
            total = min(args.crc << 20, image.size & ~511)
            while done < total:
                piece = image.read(done, min(512 * 1024, total - done))
                crc = zlib.crc32(piece, crc)
                done += len(piece)
            log(f"export {name!r}: its first {done} bytes: crc32 {crc:08x}")
    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind((args.listen, args.port))
    server.listen(4)
    log(f"listening on {args.listen}:{args.port}")
    while True:
        sock, peer = server.accept()
        client = Client(sock, f"{peer[0]}:{peer[1]}", exports, args.workers)
        log(f"{client.peer}: connected")
        if args.once:
            client.run()
            if client.requests > 0:
                return 0
            continue
        threading.Thread(target=client.run, daemon=True).start()


if __name__ == "__main__":
    sys.exit(main())
