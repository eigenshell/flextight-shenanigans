# SPDX-License-Identifier: GPL-2.0-or-later
"""Tests for tools/fwpatch.py — patched MICROCOD.3XX copies.

No vendor firmware is in the repo: tests build a SYNTHETIC 65536-byte image
(0x5A fill) with the expected stock bytes planted at each site, and pass
--allow-any-input. Offsets and stock/new bytes are pinned here as literals,
not read from the tool's tables. One test uses the real stock file and is
skipped when it is absent.
"""

import hashlib
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).parent.parent
TOOL = ROOT / "tools" / "fwpatch.py"
REAL_STOCK = Path(
    "/home/hugo/Projects/flexcolor_decomp/extracted/fc403_data/"
    "Program_Executable_Files/Firmware/MICROCOD.3XX")
STOCK_SHA256 = "d8d7188574c52b255cf7940bef7cc9192f744693aabc1f735758b7fecdec65f3"

# (bin, offset, stock code) — pinned literally.
BIN_SITES = [
    (1, 0x18BC, 0x00),
    (2, 0x18C2, 0x01),
    (3, 0x18C8, 0x02),
    (4, 0x18CE, 0x03),
    (6, 0x18D4, 0x04),
    (12, 0x18DA, 0x06),
]


def synthetic():
    img = bytearray([0x5A]) * 65536
    img[0xDF20] = 0xFF
    for _, off, code in BIN_SITES:
        img[off - 3:off + 1] = bytes([0xC6, 0x46, 0xFF, code])
    for off in (0x16E3, 0x2C2C):
        img[off - 1:off + 1] = bytes([0xB0, 0x45])
    return bytes(img)


@pytest.fixture
def stock(tmp_path):
    p = tmp_path / "stock.bin"
    p.write_bytes(synthetic())
    return p


def run(*args):
    return subprocess.run([sys.executable, str(TOOL), *map(str, args)],
                          capture_output=True, text=True)


def build(stock, out, *patches, any_input=True):
    extra = ["--allow-any-input"] if any_input else []
    return run("build", "-o", out, "--in", stock, *extra, *patches)


def diff(a, b):
    return [(i, a[i], b[i]) for i in range(len(a)) if a[i] != b[i]]


def test_stock_hash_pinned():
    sys.path.insert(0, str(ROOT))
    from tools import fwpatch
    assert fwpatch.STOCK_SHA256 == STOCK_SHA256
    assert fwpatch.IMAGE_SIZE == 65536


def test_list():
    r = run("list")
    assert r.returncode == 0
    for s in ("e0-marker", "0xDF20", "bincode", "0x18DA", "demand-dma",
              "0x16E3", "0x2C2C"):
        assert s in r.stdout


def test_e0_marker(stock, tmp_path):
    out = tmp_path / "e0.bin"
    r = build(stock, out, "e0-marker")
    assert r.returncode == 0, r.stderr
    assert diff(synthetic(), out.read_bytes()) == [(0xDF20, 0xFF, 0xA5)]
    m = (tmp_path / "e0.bin.txt").read_text()
    assert "ftpoc upload e0.bin" in m
    assert "ftpoc maint peek 3000:DF20 1" in m
    assert ("power-cycle restores stock; never send WriteBuffer flash "
            "sub-cmds 0x10/0x20/0x30/0x50") in m
    assert hashlib.sha256(synthetic()).hexdigest() in m
    assert hashlib.sha256(out.read_bytes()).hexdigest() in m
    assert "0xDF20 FF -> A5" in m


@pytest.mark.parametrize("b,off,old", BIN_SITES)
def test_bincode_each_site(stock, tmp_path, b, off, old):
    out = tmp_path / "o.bin"
    r = build(stock, out, "bincode:%d=0x0F" % b)
    assert r.returncode == 0, r.stderr
    assert diff(synthetic(), out.read_bytes()) == [(off, old, 0x0F)]


def test_bincode_bin12_code5(stock, tmp_path):
    out = tmp_path / "o.bin"
    assert build(stock, out, "bincode:12=5").returncode == 0
    assert diff(synthetic(), out.read_bytes()) == [(0x18DA, 0x06, 0x05)]


@pytest.mark.parametrize("spec", ["bincode:12=16", "bincode:12=-1",
                                  "bincode:8=5", "bincode:12", "bincode:x=1",
                                  "nonsense"])
def test_bad_specs(stock, tmp_path, spec):
    out = tmp_path / "o.bin"
    r = build(stock, out, spec)
    assert r.returncode != 0
    assert not out.exists()


def test_demand_dma(stock, tmp_path):
    out = tmp_path / "o.bin"
    assert build(stock, out, "demand-dma").returncode == 0
    assert diff(synthetic(), out.read_bytes()) == [
        (0x16E3, 0x45, 0x05), (0x2C2C, 0x45, 0x05)]


def test_block_mode_refused():
    sys.path.insert(0, str(ROOT))
    from tools import fwpatch
    s = fwpatch.Site("x", 0x16E3, 0x45, 0x85, bytes([0xB0]))
    orig = fwpatch.parse_patch
    try:
        fwpatch.parse_patch = lambda spec: [s]
        with pytest.raises(fwpatch.PatchError, match="block mode"):
            fwpatch.apply_patches(synthetic(), ["x"])
    finally:
        fwpatch.parse_patch = orig


def test_combined(stock, tmp_path):
    out = tmp_path / "o.bin"
    r = build(stock, out, "e0-marker", "bincode:12=7", "demand-dma")
    assert r.returncode == 0, r.stderr
    assert diff(synthetic(), out.read_bytes()) == [
        (0x16E3, 0x45, 0x05), (0x18DA, 0x06, 0x07), (0x2C2C, 0x45, 0x05),
        (0xDF20, 0xFF, 0xA5)]


@pytest.mark.parametrize("patches", [("e0-marker", "e0-marker"),
                                     ("bincode:12=5", "bincode:12=7"),
                                     ("demand-dma", "demand-dma")])
def test_same_site_twice(stock, tmp_path, patches):
    out = tmp_path / "o.bin"
    r = build(stock, out, *patches)
    assert r.returncode != 0
    assert "twice" in r.stderr
    assert not out.exists()


@pytest.mark.parametrize("off,val,spec", [
    (0xDF20, 0x00, "e0-marker"),
    (0x18DA, 0x05, "bincode:12=7"),
    (0x18D9, 0x00, "bincode:12=7"),   # FF of C6 46 FF
    (0x18D7, 0x90, "bincode:12=7"),   # C6
    (0x16E3, 0x85, "demand-dma"),
    (0x2C2C, 0x05, "demand-dma"),
    (0x2C2B, 0x90, "demand-dma"),     # B0 of mov al
])
def test_stock_mismatch_refused(tmp_path, off, val, spec):
    img = bytearray(synthetic())
    img[off] = val
    p = tmp_path / "s.bin"
    p.write_bytes(img)
    out = tmp_path / "o.bin"
    r = build(p, out, spec)
    assert r.returncode != 0
    assert "refusing" in r.stderr
    assert not out.exists()


def test_wrong_size_refused(tmp_path):
    p = tmp_path / "s.bin"
    p.write_bytes(synthetic() + b"\x00")
    out = tmp_path / "o.bin"
    r = build(p, out, "e0-marker")
    assert r.returncode != 0
    assert not out.exists()


def test_non_stock_hash_refused(stock, tmp_path):
    out = tmp_path / "o.bin"
    r = build(stock, out, "e0-marker", any_input=False)
    assert r.returncode != 0
    assert "sha256" in r.stderr
    assert not out.exists()


def test_verify(stock, tmp_path):
    out = tmp_path / "o.bin"
    assert build(stock, out, "e0-marker", "bincode:12=5").returncode == 0
    r = run("verify", out, "--in", stock, "--allow-any-input")
    assert r.returncode == 0, r.stdout + r.stderr
    assert "0x18DA 06 -> 05" in r.stdout
    assert "0xDF20 FF -> A5" in r.stdout
    assert "2 byte(s) differ" in r.stdout


def test_verify_outside_sites(stock, tmp_path):
    img = bytearray(synthetic())
    img[0xDF20] = 0xA5
    img[0x1234] ^= 0xFF
    p = tmp_path / "o.bin"
    p.write_bytes(img)
    r = run("verify", p, "--in", stock, "--allow-any-input")
    assert r.returncode != 0
    assert "0x1234" in r.stdout and "OUTSIDE" in r.stdout


def test_verify_block_mode_flagged(stock, tmp_path):
    img = bytearray(synthetic())
    img[0x16E3] = 0x85
    p = tmp_path / "o.bin"
    p.write_bytes(img)
    r = run("verify", p, "--in", stock, "--allow-any-input")
    assert r.returncode != 0


@pytest.mark.skipif(not REAL_STOCK.exists(), reason="stock MICROCOD.3XX absent")
def test_real_stock(tmp_path):
    data = REAL_STOCK.read_bytes()
    assert hashlib.sha256(data).hexdigest() == STOCK_SHA256
    out = tmp_path / "e0.bin"
    r = run("build", "-o", out, "--in", REAL_STOCK, "e0-marker",
            "bincode:12=5", "demand-dma")
    assert r.returncode == 0, r.stderr
    assert diff(data, out.read_bytes()) == [
        (0x16E3, 0x45, 0x05), (0x18DA, 0x06, 0x05), (0x2C2C, 0x45, 0x05),
        (0xDF20, 0xFF, 0xA5)]
