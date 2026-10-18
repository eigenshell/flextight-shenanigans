#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""A stand-in for ftpoc on Linux, for ftwebd's tests and a local demo of
the page: `frames ... --progress -o BASE` and `scan --job JOB --progress`
print ftpoc's progress lines (libft/ft_progress.h) and write synthetic
files; SIGINT (ftwebd's stop) ends the run as `progress end stopped`.
Every call appends its argv (JSON) to fake_ftpoc.log in the cwd.

`frames --detail ...` (the ARC chain) writes its job the same way.

FAKE_FTPOC_DELAY: seconds per progress line (default 0.01).
FAKE_FTPOC_WINDOW: a window scan's base (libft/test/window_dump: <w>.raw,
.raw.cal, .raw.txt); scan copies it as each window, the window's first
frame as its share_frame, so the page can cut and print real pixels.
Otherwise windows are 64 zero bytes. FAKE_FTPOC_PREVIEW: a preview's base
(<p>.raw, .raw.cal, .raw.txt) frames writes as strip.raw while it
"scans" (each progress line a share of the bytes, as ftpoc writes the
lines as they come), then the rest (else 64 zero bytes). scan says what focus does: window 1 a bracket, the rest carry
it."""
import json
import os
import re
import shutil
import signal
import struct
import sys
import time
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent
DELAY = float(os.environ.get("FAKE_FTPOC_DELAY", "0.01"))
stopped = False


def on_int(signum, frame):
    global stopped
    stopped = True


def say(line):
    print(line, flush=True)


def png(path, w, h, boxes):
    """grey strip, darker film frames (boxes in px: x, y, w, h)"""
    row = bytearray(b"\x80" * (w * 3))
    rows = []
    for y in range(h):
        r = bytearray(row)
        for bx, by, bw, bh in boxes:
            if by <= y < by + bh:
                r[bx * 3:(bx + bw) * 3] = b"\x30\x28\x20" * bw
        rows.append(b"\x00" + bytes(r))
    raw = zlib.compress(b"".join(rows), 6)

    def chunk(t, d):
        return (struct.pack(">I", len(d)) + t + d +
                struct.pack(">I", zlib.crc32(t + d) & 0xffffffff))
    Path(path).write_bytes(
        b"\x89PNG\r\n\x1a\n" +
        chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
        chunk(b"IDAT", raw) + chunk(b"IEND", b""))


def lines(total, on=None):
    for k in range(50, total + 1, 50):
        if stopped:
            return False
        if on:
            on(k, total)
        say("progress lines %d/%d" % (k, total))
        time.sleep(DELAY)
    say("progress lines %d/%d" % (total, total))
    return True


def frames(args):
    base = args[args.index("-o") + 1]
    say("progress begin preview 1")
    say("progress unit preview 1 1")
    pv = os.environ.get("FAKE_FTPOC_PREVIEW")
    data = Path(pv + ".raw").read_bytes() if pv else b""
    out = open(base + ".raw", "wb") if pv else None

    def grow(k, total):
        out.write(data[out.tell():len(data) * k // total])
        out.flush()
    ok = lines(4795, grow if pv else None)
    if out:
        out.write(data[out.tell():])
        out.close()
    if not ok:
        say("progress end stopped")
        return 1
    if pv:
        for ext in (".raw.cal", ".raw.txt"):
            shutil.copy(pv + ext, base + ext)
    else:
        Path(base + ".raw").write_bytes(b"\0" * 64)
        Path(base + ".raw.txt").write_text("px=666\nx=2\nbin=12\n")
    say("progress file " + base + ".raw")
    job = (HERE / "preview.job").read_text()
    Path(base + ".job").write_text(job)
    say("progress file " + base + ".job")
    # ~525 ppi, the frames of preview.job (y0 along, x0 across)
    s = 525 / 25.4
    boxes = [(int(float(m[0]) * s), int(float(m[1]) * s),
              int(float(m[2]) * s), int(float(m[3]) * s))
             for m in re.findall(r"window = (\S+) (\S+) (\S+) (\S+)", job)]
    png(base + ".png", 4795, 666, boxes)
    say("progress file " + base + ".png")
    print("wrote %s.job and %s.png" % (base, base))
    say("progress end ok")
    return 0


def sections(text):
    """[(frame number, include, group, start, end)] of a job's text"""
    out = []
    heads = list(re.finditer(r"^\[frame (\d+)\]", text, re.M)) + [None]
    for h, nxt in zip(heads, heads[1:]):
        end = nxt.start() if nxt else len(text)
        body = text[h.start():end]
        inc = re.search(r"^include = (\w+)", body, re.M).group(1) == "yes"
        g = re.search(r"^group = (\d+)", body, re.M)
        out.append((int(h.group(1)), inc, int(g.group(1)) if g else 0,
                    h.start(), end))
    return out


def scan(args):
    job = args[args.index("--job") + 1]
    text = Path(job).read_text()
    secs = sections(text)
    groups = sorted({g for _, inc, g, _, _ in secs if inc and g > 0})
    base = job[:-4] if job.endswith(".job") else job
    win = os.environ.get("FAKE_FTPOC_WINDOW")
    say("progress begin window %d" % len(groups))
    for i, g in enumerate(groups, 1):
        say("progress unit window %d %d" % (i, g))
        first = min(n for n, inc, gg, _, _ in secs if inc and gg == g)
        if i == 1:
            for k, (o, s) in enumerate(((-500, 0.031), (0, 0.044),
                                        (500, 0.040)), 1):
                say("progress focus %d frame %d at %+d" % (k, first, o))
                time.sleep(DELAY)
                say("progress sharp %+d %.4f" % (o, s))
            say("progress focused +250 measured")
        else:
            say("progress focused +250 carried")
        if not lines(1890):
            say("progress end stopped")
            return 1
        raw = "%s_win%02d.raw" % (base, g)
        if win:
            shutil.copy(win + ".raw", raw)
            shutil.copy(win + ".raw.cal", raw + ".cal")
            meta = Path(win + ".raw.txt").read_text()
            Path(raw + ".txt").write_text(re.sub(
                r"share_frame=\d+ ", "share_frame=%d " % first, meta))
        else:
            Path(raw).write_bytes(b"\0" * 64)
            Path(raw + ".txt").write_text("share=2450\n")
        # the window's frames are done, as ftpoc marks them in the job
        for n, inc, gg, a, b in reversed(sections(text)):
            if inc and gg == g:
                text = text[:a] + text[a:b].replace("done = no",
                                                    "done = yes") + text[b:]
        Path(job).write_text(text)
        say("progress file " + raw)
    say("progress end ok")
    return 0


def detail(args):
    base = args[args.index("-o") + 1]
    say("progress begin preview 1")
    say("progress unit preview 1 1")
    if not lines(4795):
        say("progress end stopped")
        return 1
    Path(base + ".job").write_text((HERE / "preview.job").read_text())
    say("progress file " + base + ".job")
    say("progress end ok")
    return 0


def main():
    signal.signal(signal.SIGINT, on_int)
    with open("fake_ftpoc.log", "a") as f:
        f.write(json.dumps(sys.argv[1:]) + "\n")
    args = sys.argv[1:]
    if args and args[0] == "frames" and "--detail" in args:
        return detail(args)
    if args and args[0] == "frames":
        return frames(args)
    if args and args[0] == "scan":
        return scan(args)
    print("fake ftpoc: unknown command", file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main())
