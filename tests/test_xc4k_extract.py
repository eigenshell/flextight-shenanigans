# SPDX-License-Identifier: GPL-2.0-or-later
"""Tests for tools/xc4k_extract.py — XC4006E streams embedded in microcode.

No vendor firmware is in the repo: tests build a SYNTHETIC stream and embed
it in filler bytes. Geometry, offsets and hashes are pinned here as literals,
not read from the tool. One test uses the real MICROCOD.3XX and is skipped
when it is absent.
"""

import hashlib
import random
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).parent.parent
TOOL = ROOT / "tools" / "xc4k_extract.py"
REAL_3XX = Path(
    "/home/hugo/Projects/flexcolor_decomp/extracted/fc403_data/"
    "Program_Executable_Files/Firmware/MICROCOD.3XX")
REAL_3XX_SHA256 = "d8d7188574c52b255cf7940bef7cc9192f744693aabc1f735758b7fecdec65f3"
# (offset, sha256 of the 14980 embedded bytes)
REAL_IMAGES = [
    (0x68D4, "8cbf5681e9541ecc9a1fe42d75f0ca7f003940040c6e382371a47e1155805360"),
    (0xA358, "bbbae76e2998f241241a448e696b151118845b2084facbbdaa3b444f4b63418a"),
]


def stream_bits(seed=1, bad_frame=None, length=119833):
    rnd = random.Random(seed)
    hdr = "11111111" + "0010" + f"{length:024b}" + "1111"
    frames = []
    for i in range(644):
        start = "1" if i == bad_frame else "0"
        frames.append(start + "".join(rnd.choice("01") for _ in range(185)))
    return hdr + "".join(frames) + "011111111", frames


def to_bytes(bits):
    bits += "1" * (-len(bits) % 8)
    return bytes(int(bits[i:i + 8], 2) for i in range(0, len(bits), 8))


def blob(stream, at=0x1234, size=0x8000):
    data = bytearray([0x5A]) * size
    data[at:at + len(stream)] = stream
    return bytes(data)


def run(*args):
    return subprocess.run([sys.executable, str(TOOL), *map(str, args)],
                          capture_output=True, text=True)


def test_synthetic_stream_is_14980_bytes():
    bits, _ = stream_bits()
    assert len(bits) == 119833
    assert len(to_bytes(bits)) == 14980


def test_scan_finds_embedded_stream(tmp_path):
    bits, _ = stream_bits()
    p = tmp_path / "fw.bin"
    p.write_bytes(blob(to_bytes(bits)))
    r = run("scan", p)
    assert r.returncode == 0
    assert "0x1234" in r.stdout


@pytest.mark.parametrize("kw", [{"bad_frame": 300}, {"length": 119832}])
def test_scan_rejects_bad_framing(tmp_path, kw):
    bits, _ = stream_bits(**kw)
    p = tmp_path / "fw.bin"
    p.write_bytes(blob(to_bytes(bits)))
    assert run("scan", p).stdout == ""


def test_extract_writes_exact_bytes_and_one_rbt_line_per_frame(tmp_path):
    bits, frames = stream_bits(seed=7)
    raw = to_bytes(bits)
    p = tmp_path / "fw.bin"
    p.write_bytes(blob(raw))
    out = tmp_path / "out"
    r = run("extract", "-o", out, p)
    assert r.returncode == 0, r.stderr
    assert (out / "fw.bin_1234.bin").read_bytes() == raw
    lines = (out / "fw.bin_1234.rbt").read_text().splitlines()
    body = [l for l in lines if set(l) <= {"0", "1"}]
    assert len(body) == 1 + 644 + 1
    assert body[1:645] == frames
    assert "".join(body) == bits


def test_tiles_cover_16x16_clbs(tmp_path):
    bits, frames = stream_bits(seed=3)
    p = tmp_path / "fw.bin"
    p.write_bytes(blob(to_bytes(bits)))
    out = tmp_path / "out"
    run("extract", "-o", out, p)
    text = (out / "fw.bin_1234.tiles.txt").read_text()
    assert text.count("\n== c") == 256
    # c08 starts at frame 330, r08 at bit 92 (after the centre line at 91).
    head = text.split("== c08 r08")[1].splitlines()
    assert "frames 330-365  bits 92-101" in head[0]
    row = frames[330][92:102].replace("0", "#").replace("1", ".")
    assert head[1] == "330 " + row


def test_no_stream_exits_1(tmp_path):
    p = tmp_path / "empty.bin"
    p.write_bytes(bytes(4096))
    r = run("extract", "-o", tmp_path / "out", p)
    assert r.returncode == 1


@pytest.mark.skipif(not REAL_3XX.exists(), reason="stock MICROCOD.3XX not present")
def test_real_microcode(tmp_path):
    assert hashlib.sha256(REAL_3XX.read_bytes()).hexdigest() == REAL_3XX_SHA256
    r = run("extract", "-o", tmp_path, REAL_3XX)
    assert r.returncode == 0, r.stderr
    for off, sha in REAL_IMAGES:
        raw = (tmp_path / f"MICROCOD.3XX_{off:04x}.bin").read_bytes()
        assert hashlib.sha256(raw).hexdigest() == sha
    assert len(list(tmp_path.glob("*.bin"))) == 2
