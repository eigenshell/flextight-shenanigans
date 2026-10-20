# SPDX-License-Identifier: GPL-2.0-or-later
"""Build patched copies of the Precision II microcode (MICROCOD.3XX).

Black-box CCD-board experiments (notes/plans/2026-10-19-blackbox-ccd-loop.md)
upload a modified microcode into the scanner's RAM: power-cycle, then
``ftpoc upload <patched.bin>`` while the scanner answers as its loader. The
image lives in RAM only; a power cycle restores stock. Flash is never
touched (never send WriteBuffer flash sub-commands 0x10/0x20/0x30/0x50).

This tool never ships firmware bytes: it reads the stock image the user
extracted from the FlexColor installer, checks it is exactly the known
stock file (65536 bytes, sha256 STOCK_SHA256), verifies the stock bytes at
every patch site, and writes a copy with the patch bytes plus a manifest.

Patches (file offsets; stock -> new):

  e0-marker          0xDF20 FF -> A5. Free space (0xFF fill); proves the
                     loader accepts a modified image. Read back after the
                     upload with ``ftpoc maint peek 3000:DF20 1``.
  bincode:<bin>=<c>  FUN_185e variant-1 immediates written to port 0x340
                     bits 4-7, one per bin. Each site is the imm8 of
                     ``C6 46 FF imm`` (mov byte [bp-1], imm). Stock codes:
                     bin1 0x18BC=00, bin2 0x18C2=01, bin3 0x18C8=02,
                     bin4 0x18CE=03, bin6 0x18D4=04, bin12 0x18DA=06.
                     Code 0..15.
  demand-dma         0x16E3 and 0x2C2C 45 -> 05: the imm8 of ``B0 45``
                     (mov al, 45h) written to DMAMOD1; single -> demand
                     transfer mode. 0x85 (block mode) is refused: it starves
                     the tick ISR.

Usage:
  fwpatch.py list
  fwpatch.py build -o OUT.bin [--in STOCK] [--allow-any-input] PATCH...
  fwpatch.py verify OUT.bin [--in STOCK] [--allow-any-input]
"""

import argparse
import hashlib
import os
import sys

DEFAULT_STOCK = (
    "/home/hugo/Projects/flexcolor_decomp/extracted/fc403_data/"
    "Program_Executable_Files/Firmware/MICROCOD.3XX"
)
IMAGE_SIZE = 65536
STOCK_SHA256 = "d8d7188574c52b255cf7940bef7cc9192f744693aabc1f735758b7fecdec65f3"

E0_OFFSET = 0xDF20
E0_STOCK = 0xFF
E0_NEW = 0xA5
E0_PEEK = "ftpoc maint peek 3000:DF20 1"

# bin -> (file offset of imm8, stock code). Each preceded by C6 46 FF.
BINCODE_SITES = {
    1: (0x18BC, 0x00),
    2: (0x18C2, 0x01),
    3: (0x18C8, 0x02),
    4: (0x18CE, 0x03),
    6: (0x18D4, 0x04),
    12: (0x18DA, 0x06),
}
BINCODE_PREFIX = bytes([0xC6, 0x46, 0xFF])

# DMAMOD1 values (imm8 of B0 xx, mov al).
DMA_SITES = (0x16E3, 0x2C2C)
DMA_PREFIX = bytes([0xB0])
DMA_STOCK = 0x45  # single transfer
DMA_DEMAND = 0x05
DMA_BLOCK = 0x85  # never: starves the tick ISR

REMINDER = ("power-cycle restores stock; never send WriteBuffer flash "
            "sub-cmds 0x10/0x20/0x30/0x50")


class PatchError(Exception):
    pass


class Site:
    """One byte write: offset, expected stock byte, new byte, and the
    instruction bytes that must precede it in the stock image."""

    def __init__(self, patch, offset, old, new, prefix=b""):
        self.patch = patch
        self.offset = offset
        self.old = old
        self.new = new
        self.prefix = prefix


def known_offsets():
    """Every offset any patch may change (verify's whitelist)."""
    offs = {E0_OFFSET} | set(DMA_SITES)
    offs |= {off for off, _ in BINCODE_SITES.values()}
    return offs


def _parse_int(s):
    s = s.strip()
    if s.lower().startswith("0x"):
        return int(s, 16)
    return int(s, 10)


def parse_patch(spec):
    """PATCH spec -> list of Site."""
    if spec == "e0-marker":
        return [Site(spec, E0_OFFSET, E0_STOCK, E0_NEW)]
    if spec == "demand-dma":
        return [Site(spec, off, DMA_STOCK, DMA_DEMAND, DMA_PREFIX)
                for off in DMA_SITES]
    if spec.startswith("bincode:"):
        body = spec[len("bincode:"):]
        if "=" not in body:
            raise PatchError("bincode: expected bincode:<bin>=<code>, got %r"
                             % spec)
        b, c = body.split("=", 1)
        try:
            b = _parse_int(b)
            c = _parse_int(c)
        except ValueError:
            raise PatchError("bincode: bad number in %r" % spec)
        if b not in BINCODE_SITES:
            raise PatchError("bincode: bin %d has no site (bins %s)" % (
                b, ", ".join(str(k) for k in sorted(BINCODE_SITES))))
        if not 0 <= c <= 15:
            raise PatchError("bincode: code %d out of range 0..15" % c)
        off, old = BINCODE_SITES[b]
        return [Site(spec, off, old, c, BINCODE_PREFIX)]
    raise PatchError("unknown patch %r (see 'fwpatch.py list')" % spec)


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def load_stock(path, allow_any=False):
    with open(path, "rb") as f:
        data = f.read()
    if len(data) != IMAGE_SIZE:
        raise PatchError("%s: %d bytes, expected %d" % (
            path, len(data), IMAGE_SIZE))
    h = sha256(data)
    if h != STOCK_SHA256 and not allow_any:
        raise PatchError("%s: sha256 %s is not stock %s "
                         "(--allow-any-input to override)" % (
                             path, h, STOCK_SHA256))
    return data


def apply_patches(stock, specs):
    """Verify every site against stock and return (patched bytes, sites).
    Raises PatchError before changing anything on any mismatch."""
    sites = []
    seen = {}
    for spec in specs:
        for s in parse_patch(spec):
            if s.offset in seen:
                raise PatchError("site 0x%04X patched twice (%s, %s)" % (
                    s.offset, seen[s.offset], spec))
            seen[s.offset] = spec
            sites.append(s)
    if not sites:
        raise PatchError("no patches given")
    for s in sites:
        if s.offset in DMA_SITES and s.new == DMA_BLOCK:
            raise PatchError("0x%04X: DMAMOD1 0x85 is block mode; refused "
                             "(starves the tick ISR)" % s.offset)
        cur = stock[s.offset]
        if cur != s.old:
            raise PatchError("%s: 0x%04X holds %02X, expected stock %02X; "
                             "refusing" % (s.patch, s.offset, cur, s.old))
        if s.prefix:
            start = s.offset - len(s.prefix)
            got = stock[start:s.offset]
            if got != s.prefix:
                raise PatchError("%s: 0x%04X preceded by %s, expected %s; "
                                 "refusing" % (s.patch, s.offset, got.hex(" "),
                                               s.prefix.hex(" ")))
    out = bytearray(stock)
    for s in sites:
        out[s.offset] = s.new
    return bytes(out), sites


def manifest(in_path, stock, out_path, out, sites):
    lines = [
        "fwpatch manifest",
        "input   %s" % in_path,
        "input sha256  %s" % sha256(stock),
        "output  %s" % out_path,
        "output sha256 %s" % sha256(out),
        "patches:",
    ]
    for s in sites:
        lines.append("  %-18s 0x%04X %02X -> %02X" % (
            s.patch, s.offset, s.old, s.new))
    lines.append("upload (after a cold boot, loader mode): ftpoc upload %s"
                 % os.path.basename(out_path))
    if any(s.offset == E0_OFFSET for s in sites):
        lines.append("E0 check: %s   (expect A5)" % E0_PEEK)
    lines.append(REMINDER)
    return "\n".join(lines) + "\n"


def cmd_list(_args):
    print("e0-marker          0x%04X %02X -> %02X  free space; proves the "
          "loader accepts a modified image (%s)" % (
              E0_OFFSET, E0_STOCK, E0_NEW, E0_PEEK))
    print("bincode:<bin>=<c>  FUN_185e variant-1 imm8 of C6 46 FF imm, port "
          "0x340 bits 4-7; code 0..15")
    for b in sorted(BINCODE_SITES):
        off, old = BINCODE_SITES[b]
        print("    bin %-2d         0x%04X stock %02X" % (b, off, old))
    print("demand-dma         %s %02X -> %02X  DMAMOD1 single -> demand "
          "(imm8 of B0 45); 0x85 block mode refused" % (
              " ".join("0x%04X" % o for o in DMA_SITES), DMA_STOCK,
              DMA_DEMAND))
    return 0


def cmd_build(args):
    try:
        stock = load_stock(args.inp, args.allow_any_input)
        out, sites = apply_patches(stock, args.patch)
    except (PatchError, OSError) as e:
        print("fwpatch: %s" % e, file=sys.stderr)
        return 1
    text = manifest(args.inp, stock, args.out, out, sites)
    with open(args.out, "wb") as f:
        f.write(out)
    with open(args.out + ".txt", "w") as f:
        f.write(text)
    sys.stdout.write(text)
    return 0


def cmd_verify(args):
    try:
        stock = load_stock(args.inp, args.allow_any_input)
        with open(args.image, "rb") as f:
            img = f.read()
    except (PatchError, OSError) as e:
        print("fwpatch: %s" % e, file=sys.stderr)
        return 1
    if len(img) != IMAGE_SIZE:
        print("fwpatch: %s: %d bytes, expected %d" % (
            args.image, len(img), IMAGE_SIZE), file=sys.stderr)
        return 1
    known = known_offsets()
    print("image   %s sha256 %s" % (args.image, sha256(img)))
    print("stock   %s sha256 %s" % (args.inp, sha256(stock)))
    bad = 0
    n = 0
    for off in range(IMAGE_SIZE):
        if img[off] == stock[off]:
            continue
        n += 1
        note = ""
        if off not in known:
            note = "  OUTSIDE KNOWN PATCH SITES"
            bad += 1
        elif off in DMA_SITES and img[off] == DMA_BLOCK:
            note = "  DMAMOD1 BLOCK MODE (refused)"
            bad += 1
        print("0x%04X %02X -> %02X%s" % (off, stock[off], img[off], note))
    print("%d byte(s) differ%s" % (n, "; %d not allowed" % bad if bad else ""))
    return 1 if bad else 0


def main(argv=None):
    ap = argparse.ArgumentParser(
        prog="fwpatch.py",
        description="Patched MICROCOD.3XX copies for RAM-only black-box "
                    "tests. " + REMINDER + ".")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("list", help="list patches")
    b = sub.add_parser("build", help="write a patched image + manifest")
    b.add_argument("-o", dest="out", required=True, help="output image")
    b.add_argument("--in", dest="inp", default=DEFAULT_STOCK,
                   help="stock MICROCOD.3XX (default: %(default)s)")
    b.add_argument("--allow-any-input", action="store_true",
                   help="skip the stock sha256 check (size still checked)")
    b.add_argument("patch", nargs="+", help="e0-marker | bincode:<bin>=<code>"
                   " | demand-dma")
    v = sub.add_parser("verify", help="diff an image against stock")
    v.add_argument("image")
    v.add_argument("--in", dest="inp", default=DEFAULT_STOCK,
                   help="stock MICROCOD.3XX (default: %(default)s)")
    v.add_argument("--allow-any-input", action="store_true",
                   help="skip the stock sha256 check (size still checked)")
    args = ap.parse_args(argv)
    return {"list": cmd_list, "build": cmd_build, "verify": cmd_verify}[
        args.cmd](args)


if __name__ == "__main__":
    sys.exit(main())
