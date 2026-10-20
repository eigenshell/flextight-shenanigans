# SPDX-License-Identifier: GPL-2.0-or-later
"""ftpoc on Linux without a scanner: the sg result mapping (test_sgio.c),
and ftpoc's read-only commands against a scripted transport
(tools/poc/test/ftio_fake.c): which device the probe keeps, and the exact
bytes each command puts on the wire. The CDBs are pinned here as
literals (they are FlexColor's, from the traces), not read from ftpoc.c."""
import os
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
POC = ROOT / "tools" / "poc"
needs_cc = pytest.mark.skipif(
    shutil.which("make") is None or shutil.which("cc") is None,
    reason="needs make and a C compiler")

INQUIRY = "120000006000"
POLL = "df0000000200"
# READ BUFFER pages, FlexColor's page burst
PAGES = ["3c00d800000000004200", "3c00d800003700000a00",
         "3c00e000040000001600", "3c00e000040000013e00",
         "3c00e000000000040000"]
# session 5 seq 64719: FlexColor's end of scan / Stop descriptor
END_WIN = ("000000000000004c00000000" + "00" * 36 +
           "0000000000e600010003e3d7"
           "000f52d600005fb400000002"
           "00001e0000010000000000b4")


@pytest.fixture(scope="module")
def fake():
    if shutil.which("make") is None or shutil.which("cc") is None:
        pytest.skip("needs make and a C compiler")
    r = subprocess.run(["make", "-C", str(POC), "test"], capture_output=True,
                       text=True)
    assert r.returncode == 0, r.stdout + r.stderr
    assert "checks ok" in r.stdout
    return POC / "ftpoc_fake"


def run(fake, tmp_path, *args, devs="micro"):
    log = tmp_path / "cmds.log"
    if log.exists():
        log.unlink()
    env = dict(os.environ, FTIO_FAKE_DEVS=devs, FTIO_FAKE_LOG=str(log))
    r = subprocess.run([str(fake), *args], capture_output=True, text=True,
                       env=env, cwd=tmp_path, timeout=30)
    cmds = [l.split() for l in log.read_text().splitlines()] \
        if log.exists() else []
    return r, cmds


def test_inquiry(fake, tmp_path):
    r, cmds = run(fake, tmp_path, "inquiry")
    assert r.returncode == 0, r.stderr
    assert "/dev/fake0: Imacon   FlexTight II     M333" in r.stdout
    assert "product: FlexTight II" in r.stdout
    assert "NOTE: loader" not in r.stdout
    # the probe's INQUIRY, then the command's: 96 bytes in, nothing else
    assert cmds == [["0", "40", "96", INQUIRY]] * 2


def test_poll(fake, tmp_path):
    r, cmds = run(fake, tmp_path, "poll", "3")
    assert r.returncode == 0, r.stderr
    assert r.stdout.count("0000  idle") == 3
    assert cmds[1:] == [["0", "40", "2", POLL]] * 3


def test_pages(fake, tmp_path):
    r, cmds = run(fake, tmp_path, "pages")
    assert r.returncode == 0, r.stderr
    assert [c[3] for c in cmds[1:]] == PAGES
    assert [c[2] for c in cmds[1:]] == ["66", "10", "22", "318", "1024"]
    assert all(c[1] == "40" for c in cmds)       # reads only
    assert "FaulPixs table (e0/000400, 318 bytes): 00 01 02" in r.stdout


def test_probe_prefers_the_microcode(fake, tmp_path):
    """loader on one node, a foreign device and a dead one, the microcode
    last: every node gets one INQUIRY (the dead one 8), the microcode's
    is kept and no upload starts"""
    r, cmds = run(fake, tmp_path, "poll", "1",
                  devs="loader,none,foreign,dead,micro")
    assert r.returncode == 0, r.stderr
    assert "loader" not in r.stderr
    probes = [c[0] for c in cmds if c[3] == INQUIRY]
    assert probes == ["0", "2"] + ["3"] * 8 + ["4"]
    assert cmds[-1] == ["4", "40", "2", POLL]
    assert "/dev/fake3: no INQUIRY answer, skipping" in r.stdout


def test_device_override(fake, tmp_path):
    r, cmds = run(fake, tmp_path, "--device", "/dev/fake1", "poll", "1",
                  devs="micro,micro")
    assert r.returncode == 0, r.stderr
    assert {c[0] for c in cmds} == {"1"}


def test_no_scanner(fake, tmp_path):
    r, cmds = run(fake, tmp_path, "poll", devs="none,foreign")
    assert r.returncode == 1
    assert "no /dev/sgN answered as Imacon" in r.stderr
    assert all(c[3] == INQUIRY for c in cmds)    # nothing else was sent


def test_loader_without_microcode_file_sends_nothing(fake, tmp_path):
    """in the loader, a command uploads first; with no MICROCOD.3XX it
    stops before any upload packet"""
    r, cmds = run(fake, tmp_path, "poll", devs="loader")
    assert r.returncode == 1
    assert "no microcode file" in r.stderr
    assert all(c[3] == INQUIRY for c in cmds)


def test_stop_sends_end_descriptor_twice(fake, tmp_path):
    r, cmds = run(fake, tmp_path, "stop")
    assert r.returncode == 0, r.stderr
    wins = [c for c in cmds if c[3].startswith("24")]
    assert len(END_WIN) == 168
    assert wins == [["0", "80", "84", "24000000000000540000", END_WIN]] * 2


def test_empty_poll_is_not_idle(fake, tmp_path):
    """SG_IO completes an underrun as a short read: an empty ScannerReady
    reply must fail the poll, not read as 00 00 (idle) from the zeroed
    buffer -- every settle and start-line wait keys off it"""
    r, cmds = run(fake, tmp_path, "poll", "3", devs="micro-empty")
    assert r.returncode == 1
    assert "poll 0: short reply (underrun)" in r.stdout
    assert "idle" not in r.stdout
    assert cmds[1:] == [["0", "40", "2", POLL]]


def test_flood_keeps_only_full_lines(fake, tmp_path):
    """On Linux every empty ReadExt of the flood completes as a 10-byte
    underrun (ff ff ff ff ff ff 00 00 00 00, the 2026-10-20 box preview:
    62045 of them). Those bytes are no line: kept, they shifted every
    later line of strip.raw and .cal, the calibration split failed and
    frames wrote no strip.job/png. The files hold full lines only, and
    the underruns print no line each (ftwebd keeps 4096 lines)."""
    r, cmds = run(fake, tmp_path, "scan", "preview", "-n", "5", "-h", "0",
                  "-o", "t.raw", devs="micro-scan")
    assert r.returncode == 0, r.stdout + r.stderr
    # 666 px x RGB x u16 = 3996-byte lines: 5 kept, 200 + 50 calibration
    raw = (tmp_path / "t.raw").read_bytes()
    cal = (tmp_path / "t.raw.cal").read_bytes()
    assert len(raw) == 19980
    assert len(cal) == 999000
    assert b"\xff\xff\xff\xff\xff\xff\x00\x00\x00\x00" not in raw + cal
    # the fake's line k starts with k & 0x7f: the lines in order, none lost
    assert [raw[k * 3996] for k in range(5)] == [122, 123, 124, 125, 126]
    assert [cal[k * 3996] for k in range(3)] == [0, 1, 2]
    assert "partial" not in r.stdout
    assert "[flood: 10 reads in 0 ms; 5 full, 5 short (50 bytes)]" in r.stdout
    assert "wrote 5 x 3996 B -> t.raw\n" in r.stdout
