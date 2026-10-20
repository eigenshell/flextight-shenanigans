# SPDX-License-Identifier: GPL-2.0-or-later
"""Extract the XC4006E CCD-board configurations embedded in Imacon microcode.

The Precision II's CCD-board FPGA has no PROM: the microcode shifts a
serial configuration stream into it at every boot (FUN_191c). The streams
sit inside the microcode files of the FlexColor installer:

  MICROCOD.3XX 0x68D4  variant 0 (4800 / Precision)
  MICROCOD.3XX 0xA358  variant 1 (Precision II / III, the one we run)
  microcod.dtp 0x5154  identical to 3XX variant 1
  Microcod.a3  0x7AB4  Progression III / 2848

Stream layout (notes/firmware/2026-10-19-fpga-variant-diff.md), MSB first:

  header  8 x 1, 0010, 24-bit length (bits incl. header), 1111   40 bits
  frames  644 x (start 0, 181 data, 4 check)                      186 bits
  tail    9 bits (0 + ones)

The check bits are a running CRC, so an edited stream must have them
recomputed or the FPGA refuses it. The tile grid (16 x 16 CLBs, 36 frames x
10 bits each, see TILE_* below) is inferred from the geometry and is not a
verified bit map.

This tool never ships firmware bytes: it reads files the user extracted
and writes into a directory of their choice (keep it out of git).

Outputs per image, named <file>_<offset>:
  .bin        the stream bytes exactly as embedded (14980 B)
  .rbt        rawbits text: header line, one line per frame, tail line
  .tiles.txt  each CLB tile as 36 rows (frames) x 10 cols, '#' = 0
              (configured; unprogrammed bits read 1), '.' = 1

Usage:
  xc4k_extract.py scan FILE...
  xc4k_extract.py extract -o DIR FILE...
"""

import argparse
import hashlib
import os
import re
import sys

HEADER_BITS = 40
FRAME_BITS = 186
FRAMES = 644
TAIL_BITS = 9
STREAM_BITS = HEADER_BITS + FRAMES * FRAME_BITS + TAIL_BITS  # 119833
STREAM_BYTES = (STREAM_BITS + 7) // 8  # 14980

# Inferred CLB grid: frame index of each column's first frame, bit index of
# each row's first bit (bit 0 = the frame's start bit).
TILE_FRAMES = 36
TILE_BITS = 10
TILE_COL_FRAME = [41 + 36 * k for k in range(8)] + [330 + 36 * k for k in range(8)]
TILE_ROW_BIT = [11 + 10 * r for r in range(8)] + [92 + 10 * r for r in range(8)]


def bits_of(data):
    return "".join(f"{x:08b}" for x in data)


def parse(stream_bits):
    """Split a stream's bits into (header, frames, tail); raise on bad framing."""
    if len(stream_bits) < STREAM_BITS:
        raise ValueError("stream truncated")
    hdr = stream_bits[:HEADER_BITS]
    if hdr[:12] != "111111110010" or hdr[36:] != "1111":
        raise ValueError("no XC4000 header")
    length = int(hdr[12:36], 2)
    if length != STREAM_BITS:
        raise ValueError(f"length {length} is not an XC4006E stream ({STREAM_BITS})")
    frames = [stream_bits[HEADER_BITS + i * FRAME_BITS:HEADER_BITS + (i + 1) * FRAME_BITS]
              for i in range(FRAMES)]
    bad = [i for i, f in enumerate(frames) if f[0] != "0"]
    if bad:
        raise ValueError(f"frame start bit not 0 at frames {bad[:5]}")
    tail = stream_bits[HEADER_BITS + FRAMES * FRAME_BITS:STREAM_BITS]
    return hdr, frames, tail


def scan(data):
    """Offsets of every valid XC4006E stream in data."""
    found = []
    for m in re.finditer(rb"\xff\x20", data):
        o = m.start()
        try:
            parse(bits_of(data[o:o + STREAM_BYTES]))
        except ValueError:
            continue
        found.append(o)
    return found


def rbt_text(hdr, frames, tail, name):
    lines = [
        "Xilinx ASCII Bitstream",
        f"Design name:\t{name} (embedded, original name unknown)",
        "Architecture:\txc4000e",
        "Part:\t4006e (inferred from frame geometry)",
        f"Bits:\t{STREAM_BITS}",
        hdr,
        *frames,
        tail,
    ]
    return "\n".join(lines) + "\n"


def tiles_text(frames, name):
    out = [f"# {name}: CLB tiles, inferred geometry (not a verified bit map)",
           "# rows = 36 frames of the tile column, cols = 10 bits of the tile row",
           "# '#' = 0 (configured), '.' = 1 (default)"]
    for c, f0 in enumerate(TILE_COL_FRAME):
        for r, b0 in enumerate(TILE_ROW_BIT):
            block = [frames[f0 + i][b0:b0 + TILE_BITS] for i in range(TILE_FRAMES)]
            zeros = sum(row.count("0") for row in block)
            out.append(f"\n== c{c:02d} r{r:02d}  frames {f0}-{f0 + TILE_FRAMES - 1}"
                       f"  bits {b0}-{b0 + TILE_BITS - 1}  configured {zeros}")
            out += [f"{f0 + i:3d} " + row.replace("0", "#").replace("1", ".")
                    for i, row in enumerate(block)]
    return "\n".join(out) + "\n"


def extract(path, outdir):
    data = open(path, "rb").read()
    base = os.path.basename(path)
    written = []
    for o in scan(data):
        raw = data[o:o + STREAM_BYTES]
        hdr, frames, tail = parse(bits_of(raw))
        stem = os.path.join(outdir, f"{base}_{o:04x}")
        name = f"{base}@0x{o:04x}"
        with open(stem + ".bin", "wb") as f:
            f.write(raw)
        with open(stem + ".rbt", "w") as f:
            f.write(rbt_text(hdr, frames, tail, name))
        with open(stem + ".tiles.txt", "w") as f:
            f.write(tiles_text(frames, name))
        written.append((name, hashlib.sha256(raw).hexdigest(), stem))
    return written


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("scan")
    s.add_argument("files", nargs="+")
    e = sub.add_parser("extract")
    e.add_argument("-o", "--out", required=True)
    e.add_argument("files", nargs="+")
    a = ap.parse_args(argv)

    if a.cmd == "scan":
        for p in a.files:
            for o in scan(open(p, "rb").read()):
                print(f"{p}\t0x{o:04x}\t{STREAM_BITS} bits")
        return 0
    os.makedirs(a.out, exist_ok=True)
    total = 0
    for p in a.files:
        for name, sha, stem in extract(p, a.out):
            print(f"{name}\tsha256 {sha}\t{stem}.{{bin,rbt,tiles.txt}}")
            total += 1
    if not total:
        print("no XC4006E streams found", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
