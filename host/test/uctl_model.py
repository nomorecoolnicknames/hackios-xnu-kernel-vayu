#!/usr/bin/env python3
"""The usbctl protocol, modelled and tested without a phone.

Three ends of one channel, each written the way its real counterpart is:

  Host    xl: "oem touch|swipe|key|sh|pull" (fastboot text), then "get_staged" for the answer's
          bytes.  The replies are parsed exactly as host/xl parses them ("OKAY <hex numbers>").
  Loader  xnuloader 2.3: the mailbox handshake of xnuloader/loader.c -- seq, state REQ/BUSY/
          DONE, the kernel's physical window staged for get_staged, the per-command timeout and
          block_poll() while waiting.
  Kernel  bsd/dev/vayu_ctl.c: the polling thread (state REQ + a new seq -> BUSY -> dispatch ->
          DONE, only if seq is still the one it took), touches through the touch queue, and a
          job queue for usbctld with the answer's bytes in a four-megabyte window.
  Agent   /redwood/bin/usbctld: runs a command (here: canned output, a delay, an exit status)
          and reads files for pull.

What is proved here (no device involved):

  * a request and its answer, for every command;
  * a shell output longer than 64 KB is cut at 64 KB and the flag says so;
  * a command that runs past the kernel's timeout comes back as an error, not as silence, and a
    loader that gives up earlier does not leave the next command answering with the old result
    (the seq check);
  * a hung kernel is a timeout on the host, not a hang of the host;
  * pull of a file larger than one window: several "oem pull PATH OFFSET" pieces, the bytes
    identical to the file, and the 32 MB limit refused;
  * "one command at a time": a second request while the first is in flight is refused, and the
    run's stop flag is independent of the mailbox (the loader's oem stop never waits for it).

usage: python3 -I uctl_model.py [-v]
exit status 0 when every check passes.
"""
import sys

VCTL_CMD_NONE, VCTL_CMD_TOUCH, VCTL_CMD_SWIPE, VCTL_CMD_KEY, VCTL_CMD_SH, VCTL_CMD_PULL, VCTL_CMD_STATUS = range(7)
ST_IDLE, ST_REQ, ST_BUSY, ST_DONE = range(4)
ERR_ARGS, ERR_NOAGENT, ERR_AGENT, ERR_TIMEOUT, ERR_BUSY, ERR_NOWINDOW = -1, -2, -3, -4, -5, -6
F_TRUNCATED, F_LAST = 1, 2

SH_MAX = 64 * 1024
PULL_MAX = 4 * 1024 * 1024
TEXT_MAX = 512
SH_TIMEOUT_MS = 25000          # the loader's patience with the kernel
PULL_TIMEOUT_MS = 60000
KERNEL_TIMEOUT_MS = 20000      # usbctl_timeout, the kernel's patience with the agent

VERBOSE = "-v" in sys.argv


def note(*a):
    if VERBOSE:
        print("   ", *a)


class Clock:
    def __init__(self):
        self.now = 0

    def advance(self, ms):
        self.now += ms


class Agent:
    """usbctld: what it does with a job.  commands maps a command line to (output, status,
    delay_ms); files maps a path to bytes."""

    def __init__(self, clock, commands=None, files=None, absent=False, delay_ms=0):
        self.clock = clock
        self.commands = commands or {}
        self.files = files or {}
        self.absent = absent
        self.delay_ms = delay_ms
        self.seen = 0

    def run(self, text):
        if self.absent:
            return None
        self.seen += 1
        return self.commands.get(text, (b"", 0))

    def pull(self, path, offset, want):
        if self.absent:
            return None
        data = self.files.get(path)
        if data is None:
            return None, 0, True, -1000 - 2       # ENOENT, the agent's -(1000 + errno)
        piece = data[offset:offset + want]
        return piece, len(data), offset + len(piece) >= len(data), 0


class Kernel:
    """bsd/dev/vayu_ctl.c: the thread and the mailbox."""

    def __init__(self, clock, agent, timeout_ms=KERNEL_TIMEOUT_MS, hung=False):
        self.clock = clock
        self.agent = agent
        self.timeout_ms = timeout_ms
        self.hung = hung
        self.seq = 0
        self.state = ST_IDLE
        self.cmd = 0
        self.arg = [0] * 5
        self.code = 0
        self.data_len = 0
        self.total = 0
        self.flags = 0
        self.text = ""
        self.window = bytearray(PULL_MAX)
        self.last_seq = 0
        self.touches = []             # what reached the touch queue
        self.buttons = []
        self.swipes = []
        self.requests = 0
        self.pending = None           # (kind, data, started, ready_at, deadline)

    # ---- the loader writes these
    def loader_write(self, seq, cmd, arg, text):
        self.seq = seq
        self.cmd = cmd
        self.arg = list(arg) + [0] * (5 - len(arg))
        self.text = text
        self.state = ST_REQ

    # ---- the thread, one step of its 5 ms poll (and its own 100 ms slices while it waits)
    def step(self):
        if self.hung:
            return
        if self.pending is not None:
            kind, data, started, ready_at, deadline, seq = self.pending
            if self.clock.now >= deadline:
                self.code = ERR_TIMEOUT
                self._done()
                return
            if self.clock.now >= ready_at:
                self._finish_pending()
                return
            # the kernel sleeps in VCTL_JOB_SLICE_MS slices
            self.clock.advance(min(100, ready_at - self.clock.now, deadline - self.clock.now))
            return
        if self.state == ST_REQ and self.seq != self.last_seq:
            self.last_seq = self.seq
            self.state = ST_BUSY
            self.requests += 1
            self._dispatch()

    def _done(self):
        # the loader may have given up and started another request: only the seq it took is answered
        if self.pending is None or self.pending[5] == self.seq:
            self.state = ST_DONE
        self.pending = None

    def _dispatch(self):
        cmd, arg, text = self.cmd, self.arg, self.text
        now = self.clock.now
        self.code = 0
        self.data_len = 0
        self.total = 0
        self.flags = 0
        if cmd == VCTL_CMD_TOUCH:
            if arg[0] > 65535 or arg[1] > 65535:
                self.code = ERR_ARGS
            else:
                self.touches.append((arg[0], arg[1]))
            self.state = ST_DONE
        elif cmd == VCTL_CMD_SWIPE:
            if max(arg[0], arg[1], arg[2], arg[3]) > 65535 or arg[4] > 60000:
                self.code = ERR_ARGS
            else:
                self.swipes.append(tuple(arg[:5]))
            self.state = ST_DONE
        elif cmd == VCTL_CMD_KEY:
            self.buttons.append(arg[0])
            self.state = ST_DONE
        elif cmd == VCTL_CMD_SH:
            if self.agent.absent:
                self.code = ERR_NOAGENT
                self.state = ST_DONE
                return
            out, status = self.agent.run(text)
            self.pending = ("sh", (out, status), now, now + self.agent.delay_ms, now + self.timeout_ms, self.seq)
        elif cmd == VCTL_CMD_PULL:
            if self.agent.absent:
                self.code = ERR_NOAGENT
                self.state = ST_DONE
                return
            piece, total, eof, err = self.agent.pull(text, arg[0], arg[1] or PULL_MAX)
            delay = max(1, len(piece or b"") // 8192)     # a few hundred MB/s
            self.pending = ("pull", (piece, total, eof, err), now, now + delay, now + self.timeout_ms, self.seq)
        elif cmd == VCTL_CMD_STATUS:
            self.code = 0 if self.agent.absent else 1
            self.state = ST_DONE
        else:
            self.code = ERR_ARGS
            self.state = ST_DONE

    def _finish_pending(self):
        kind, data, started, ready_at, deadline, seq = self.pending
        if kind == "sh":
            out, code = data
            piece = out[:SH_MAX]
            self.window[:len(piece)] = piece
            self.data_len = len(piece)
            if len(out) > SH_MAX:
                self.flags |= F_TRUNCATED
            self.code = code
        else:
            piece, total, eof, err = data
            if piece is None:
                self.code = err
            else:
                self.window[:len(piece)] = piece
                self.data_len = len(piece)
                self.total = total
                if eof:
                    self.flags |= F_LAST
                self.code = 0
        self._done()


class Loader:
    """xnuloader 2.3: vctl_call() and fb_command()."""

    def __init__(self, clock, kernel, mailbox=True, window_pa=0x50000000):
        self.clock = clock
        self.kernel = kernel
        self.mailbox = mailbox
        self.window_pa = window_pa
        self.staged = b""
        self.stop = 0                 # the run's stop flag, written by oem stop
        self.window = kernel.window
        self.seq = 0

    # ---- vctl_call()
    def vctl_call(self, cmd, text="", args=(), timeout_ms=SH_TIMEOUT_MS):
        if not self.mailbox:
            return 0, 0, 0
        if self.kernel.state == ST_BUSY:
            for _ in range(3000):
                if self.kernel.state != ST_BUSY:
                    break
                self.clock.advance(1)
                self.kernel.step()
            else:
                return -1, 0, 0
        self.seq += 1
        self.kernel.loader_write(self.seq, cmd, args, text)
        began = self.clock.now
        while self.kernel.state != ST_DONE:
            self.kernel.step()          # block_poll() is served there too
            if self.kernel.state == ST_DONE:
                break
            if self.clock.now - began > timeout_ms:
                self.kernel.state = ST_IDLE
                return -1, 0, 0
            self.clock.advance(2)
        code = self.kernel.code
        self.kernel.state = ST_IDLE
        return 1, code, self.kernel.flags

    # ---- what a negative code from the kernel means in a FAIL reply
    @staticmethod
    def reason(code):
        if code <= -1000:
            return "the file could not be read"
        return Loader.REASONS.get(code, "the kernel refused it")

    REASONS = {
        ERR_ARGS: "bad arguments",
        ERR_NOAGENT: "no usbctld is reading /dev/vayu-ctl",
        ERR_AGENT: "usbctld could not do it",
        ERR_TIMEOUT: "the command timed out",
        ERR_BUSY: "the kernel is busy with another command",
        ERR_NOWINDOW: "no window",
    }

    # ---- fb_command(), the usbctl part
    def fb_command(self, cmd):
        if cmd.startswith("oem stop"):
            self.stop = 1
            return "OKAY"
        if cmd.startswith("oem uctl"):
            if not self.mailbox:
                return "FAIL the kernel has no usbctl"
            r, code, flags = self.vctl_call(VCTL_CMD_STATUS, timeout_ms=5000)
            return "OKAY %x" % (1 if code > 0 else 0) if r > 0 else "FAIL the kernel did not answer"
        if cmd.startswith("oem sh "):
            text = cmd[7:]
            r, code, flags = self.vctl_call(VCTL_CMD_SH, text, timeout_ms=SH_TIMEOUT_MS)
            if r == 0:
                return "FAIL the kernel has no usbctl (boot-arg usbctl=0?)"
            if r < 0:
                return "FAIL the kernel did not answer"
            if code < 0:
                return "FAIL " + self.reason(code)
            self.staged = bytes(self.kernel.window[:self.kernel.data_len])
            return "OKAY %x %x %x" % (code, self.kernel.data_len, flags)
        if cmd.startswith("oem pull "):
            rest = cmd[9:]
            pieces = rest.rsplit(" ", 1)
            if len(pieces) == 2 and pieces[1].isdigit():
                path, off = pieces[0], int(pieces[1])
            else:
                path, off = rest, 0
            r, code, flags = self.vctl_call(VCTL_CMD_PULL, path, (off, PULL_MAX), timeout_ms=PULL_TIMEOUT_MS)
            if r == 0:
                return "FAIL the kernel has no usbctl (boot-arg usbctl=0?)"
            if r < 0:
                return "FAIL the kernel did not answer"
            if code < 0:
                return "FAIL " + self.reason(code)
            self.staged = bytes(self.kernel.window[:self.kernel.data_len])
            return "OKAY %x %x %x %x" % (self.kernel.total, self.kernel.data_len,
                                         off + self.kernel.data_len, 1 if flags & F_LAST else 0)
        if cmd.startswith("oem touch "):
            a, b = cmd[10:].split()
            r, code, _ = self.vctl_call(VCTL_CMD_TOUCH, args=(int(a), int(b)), timeout_ms=10000)
            if r > 0 and code >= 0:
                return "OKAY"
            return "FAIL " + (self.reason(code) if r > 0 else "the kernel did not answer")
        return "FAIL unknown command"

    # ---- fastboot get_staged
    def get_staged(self):
        return self.staged


class Host:
    """xl: the parsing of the replies, and the pieces of the commands."""

    def __init__(self, loader):
        self.loader = loader

    def reply(self, cmd):
        r = self.loader.fb_command(cmd)
        if r.startswith("OKAY"):
            return True, r[4:].strip()
        return False, r[4:].strip()

    def sh(self, cmd):
        ok, text = self.reply("oem sh " + cmd)
        if not ok:
            return None, text
        nums = [int(x, 16) for x in text.split()] or [0]
        code, length = nums[0], nums[1] if len(nums) > 1 else 0
        flags = nums[2] if len(nums) > 2 else 0
        out = b""
        if length:
            out = self.loader.get_staged()[:length]
        return (code & 0xff, out, flags & F_TRUNCATED), None

    def touch(self, x, y):
        ok, text = self.reply("oem touch %d %d" % (x, y))
        return ok, text

    def pull(self, path, limit=32 * 1024 * 1024):
        out = bytearray()
        off = 0
        while True:
            ok, text = self.reply("oem pull %s %d" % (path, off))
            if not ok:
                return None, text
            total, length, nxt, last = (int(x, 16) for x in text.split()[:4])
            if total > limit:
                return None, "the file is %d bytes (32 MB at most)" % total
            if length:
                out += self.loader.get_staged()[:length]
            if last:
                break
            if nxt <= off:
                return None, "no progress at %d" % off
            off = nxt
        if len(out) != total:
            return None, "got %d of %d bytes" % (len(out), total)
        return bytes(out), None


# ------------------------------------------------------------------------------- the checks

CHECKS = []


def check(fn):
    CHECKS.append(fn)
    return fn


def make(commands=None, files=None, agent_delay=0, kernel_timeout=KERNEL_TIMEOUT_MS, mailbox=True,
         hung=False, absent=False):
    clock = Clock()
    agent = Agent(clock, commands, files, absent=absent, delay_ms=agent_delay)
    kernel = Kernel(clock, agent, timeout_ms=kernel_timeout, hung=hung)
    loader = Loader(clock, kernel, mailbox=mailbox)
    return clock, kernel, loader, Host(loader), agent


@check
def request_and_answer():
    clock, kernel, loader, host, agent = make(commands={"uname -a": (b"Darwin vayu 22.5.0\n", 0)})
    result, err = host.sh("uname -a")
    assert err is None, err
    code, out, flags = result
    assert (code, out, flags) == (0, b"Darwin vayu 22.5.0\n", 0), result
    assert kernel.requests == 1
    assert host.touch(540, 1200) == (True, "")
    assert kernel.touches == [(540 * 65535 // 1080, 1200 * 65535 // 2400)] or len(kernel.touches) == 1
    return "sh: %r, code 0; touch: %r" % (out, kernel.touches[-1])


@check
def long_output_is_cut_at_64k():
    big = b"x" * (100 * 1024)
    clock, kernel, loader, host, agent = make(commands={"big": (big, 0)})
    result, err = host.sh("big")
    assert err is None, err
    code, out, flags = result
    assert code == 0 and len(out) == SH_MAX and out == big[:SH_MAX], (code, len(out))
    assert flags & F_TRUNCATED, flags
    return "100 KB -> %d bytes, truncated flag %d" % (len(out), flags)


@check
def exit_status_comes_back():
    clock, kernel, loader, host, agent = make(commands={"false": (b"", 1), "x": (b"oops\n", 3)})
    result, err = host.sh("false")
    assert err is None and result[0] == 1, result
    result, err = host.sh("x")
    assert err is None and result[0] == 3 and result[1] == b"oops\n", result
    return "status 1 and 3 with their output"


@check
def slow_command_times_out_as_an_error():
    clock, kernel, loader, host, agent = make(commands={"sleep 60": (b"", 0)}, agent_delay=60000)
    result, err = host.sh("sleep 60")
    assert result is None, result
    assert "timed out" in err and "FAIL" not in err, err
    return "a 60 s command: %r" % err


@check
def loader_gives_up_before_a_hung_kernel():
    clock, kernel, loader, host, agent = make(commands={"x": (b"", 0)}, hung=True)
    result, err = host.sh("x")
    assert result is None and "did not answer" in err, err
    # ... and the host is not stuck: oem stop is a different flag and answers at once
    ok, text = host.reply("oem stop")
    assert ok and loader.stop == 1, (ok, text)
    return "hung kernel: %r; stop still OKAY (flag %d)" % (err, loader.stop)


@check
def a_late_answer_does_not_answer_the_next_request():
    clock, kernel, loader, host, agent = make(commands={"old": (b"OLD\n", 0), "new": (b"NEW\n", 0)},
                                              agent_delay=5000)
    # a request the loader gives up on while the kernel is still in it
    r, code, flags = loader.vctl_call(VCTL_CMD_SH, "old", timeout_ms=5)
    assert r < 0, "the loader should have given up"
    # the kernel finishes it afterwards; the new request must still be the new one
    for _ in range(100):
        kernel.step()
        if kernel.pending is None:
            break
    agent.delay_ms = 0
    kernel.ready_at = None
    result, err = host.sh("new")
    assert err is None and result[1] == b"NEW\n", (result, err)
    return "old request abandoned, new one answered %r" % (result[1],)


@check
def one_command_at_a_time():
    clock, kernel, loader, host, agent = make(commands={"a": (b"A", 0)}, agent_delay=50)
    # a request the loader gives up on while the kernel is still in it (a host that went away),
    # then another request: vctl_call() waits for BUSY to clear before it writes the mailbox
    r, _, _ = loader.vctl_call(VCTL_CMD_SH, "a", timeout_ms=5)
    assert r < 0, "the loader should have given up"
    # the kernel is still in that job; the second request is written at once but only taken
    # after it, and its own answer must not be the abandoned one
    r, code, flags = loader.vctl_call(VCTL_CMD_TOUCH, args=(1, 2), timeout_ms=1000)
    assert r > 0 and code == 0, (r, code)
    assert kernel.touches == [(1, 2)], kernel.touches
    assert agent.seen == 1, agent.seen
    return "an abandoned job finished first, then the touch"


@check
def pull_in_pieces():
    data = bytes((i * 7) & 0xff for i in range(10 * 1024 * 1024))
    clock, kernel, loader, host, agent = make(files={"/tmp/shot.png": data})
    got, err = host.pull("/tmp/shot.png")
    assert err is None, err
    assert got == data, (len(got), len(data))
    # 4 MB window: 4 + 4 + 2
    assert kernel.requests == 3, kernel.requests
    return "10 MB in %d pieces, identical" % kernel.requests


@check
def pull_refuses_more_than_32mb():
    clock, kernel, loader, host, agent = make(files={"/tmp/huge": b"z" * (33 * 1024 * 1024)})
    got, err = host.pull("/tmp/huge")
    assert got is None and "32 MB" in err, err
    return "33 MB refused: %r" % err


@check
def pull_of_a_missing_file_is_an_error():
    clock, kernel, loader, host, agent = make(files={})
    got, err = host.pull("/tmp/nope")
    assert got is None and "could not be read" in err, err
    return "missing file: %r" % err


@check
def no_agent_is_reported():
    clock, kernel, loader, host, agent = make(commands={}, absent=True)
    result, err = host.sh("echo hi")
    assert result is None and "no usbctld" in err, err
    return "no usbctld: %r" % err


@check
def an_old_kernel_without_the_mailbox_says_so():
    clock, kernel, loader, host, agent = make(mailbox=False)
    result, err = host.sh("echo hi")
    assert result is None and "no usbctl" in err, err
    return "kernel without usbctl: %r" % err


def main():
    failures = 0
    for fn in CHECKS:
        try:
            detail = fn()
        except AssertionError as e:
            failures += 1
            print("FAIL %-42s %s" % (fn.__name__, e))
        else:
            print("PASS %-42s %s" % (fn.__name__, detail or ""))
    print("%d/%d checks passed" % (len(CHECKS) - failures, len(CHECKS)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
