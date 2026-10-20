# SPDX-License-Identifier: GPL-2.0-or-later
"""MICROCOD.3XX upload: wire bytes and flow, no hardware.

Every CDB/header literal below was produced by compiling and running the C
reference (libft ft_upload_chunk / ft_upload_trailer, hardware-proven and
matching the FlexColor session 2/3 SRBs), not by the Python under test:

    chunk  i len 4106 cdb 3b010000000000100a00 hdr 01 3i 0000000000000000
    trailer len 10 cdb 3b010000000000000a00 data 003001000e0000000000
"""

import json

import pytest

from flextight.cli import main
from flextight.transport import FakeTransport, Step, step_to_dict
from flextight import upload as up

CHUNK_CDB = "3b010000000000100a00"
CHUNK_HDR = [
    "01300000000000000000", "01310000000000000000", "01320000000000000000",
    "01330000000000000000", "01340000000000000000", "01350000000000000000",
    "01360000000000000000", "01370000000000000000", "01380000000000000000",
    "01390000000000000000", "013a0000000000000000", "013b0000000000000000",
    "013c0000000000000000", "013d0000000000000000", "013e0000000000000000",
    "013f0000000000000000",
]
TRAILER_CDB = "3b010000000000000a00"
TRAILER_DATA = "003001000e0000000000"
INQUIRY = "120000002400"
D800 = "3c00d800000000004200"
POLL = "df0000000200"

LOADER_INQ = b"\x06\x00\x02\x02\x1f\x00\x00\x00Imacon  SCSI Loader     L302"
MICRO_INQ = b"\x06\x00\x02\x02\x1f\x00\x00\x00Imacon  FlexTight II    M333"  # as read on hardware 2026-10-20

FW = bytes((i * 7 + 3) & 0xFF for i in range(65536))


def _chunks(fw=FW, n=16):
    return [Step(CHUNK_CDB, "out",
                 data=bytes.fromhex(CHUNK_HDR[i]) + fw[i * 4096:(i + 1) * 4096])
            for i in range(n)]


def _happy_steps():
    return ([Step(INQUIRY, "in", response=LOADER_INQ),
             Step(D800, "in", response=bytes(0x42))]
            + _chunks()
            + [Step(TRAILER_CDB, "out", data=bytes.fromhex(TRAILER_DATA)),
               Step(POLL, "in", response=b"\x04\x00"),
               Step(POLL, "in", response=b"\x04\x00"),
               Step(POLL, "in", response=b"\x00\x00"),
               Step(INQUIRY, "in", response=MICRO_INQ)])


def _run(steps, fw=FW, **kw):
    t = FakeTransport(steps)
    kw.setdefault("nodes", list)
    kw.setdefault("sleep", lambda s: None)
    logs = []
    kw.setdefault("log", logs.append)
    return t, up.upload(t, fw, device="/dev/sgX", **kw), logs


def test_packets_match_c_reference():
    code = bytes(range(256)) * 16
    for i in range(16):
        cdb, data = up.chunk_packet(i, code)
        assert cdb.hex() == CHUNK_CDB
        assert data[:10].hex() == CHUNK_HDR[i]
        assert data[10:] == code and len(data) == 4106
    cdb, data = up.trailer_packet()
    assert cdb.hex() == TRAILER_CDB
    assert data.hex() == TRAILER_DATA
    with pytest.raises(ValueError):
        up.chunk_packet(16, code)
    with pytest.raises(ValueError):
        up.chunk_packet(0, code[:-1])


def test_loader_identity():
    assert up.is_loader(LOADER_INQ)
    assert not up.is_loader(MICRO_INQ)
    assert not up.is_loader(LOADER_INQ[:26])
    assert up.is_microcode(MICRO_INQ)
    assert not up.is_microcode(LOADER_INQ)


def test_happy_path_sends_exact_sequence():
    t, (path, inq), logs = _run(_happy_steps())
    assert t._pos == len(t._steps)          # every step consumed, in order
    assert path == "/dev/sgX" and inq == MICRO_INQ
    assert any("FlexTight II" in m for m in logs)


def test_refuses_when_not_loader():
    t, steps = None, [Step(INQUIRY, "in", response=MICRO_INQ)]
    with pytest.raises(up.UploadError, match="not the loader"):
        _run(steps)


def test_refuses_wrong_size():
    steps = [Step(INQUIRY, "in", response=LOADER_INQ)]
    t = FakeTransport(steps)
    with pytest.raises(up.UploadError, match="65536"):
        up.upload(t, FW[:-1], nodes=list, sleep=lambda s: None, log=lambda m: None)
    assert t._pos == 1                      # nothing but INQUIRY sent


def test_d800_failure_is_not_fatal():
    steps = _happy_steps()
    steps[1] = Step(D800, "in", status=2)
    t, (path, inq), _ = _run(steps)
    assert t._pos == len(t._steps)


def test_chunk_failure_never_sends_trailer():
    steps = ([Step(INQUIRY, "in", response=LOADER_INQ),
              Step(D800, "in", response=bytes(0x42))]
             + _chunks(n=5)
             + [Step(CHUNK_CDB, "out", status=2),
                Step(TRAILER_CDB, "out", data=bytes.fromhex(TRAILER_DATA))])
    t = FakeTransport(steps)
    with pytest.raises(up.UploadError, match="no run trailer"):
        up.upload(t, FW, nodes=list, sleep=lambda s: None, log=lambda m: None)
    assert t._pos == len(steps) - 1         # trailer step left unconsumed


def test_poll_errors_then_microcode_on_other_node():
    # the loader node stops answering after the trailer; the microcode
    # shows up on another node, found by an INQUIRY-only probe
    steps = ([Step(INQUIRY, "in", response=LOADER_INQ),
              Step(D800, "in", response=bytes(0x42))]
             + _chunks()
             + [Step(TRAILER_CDB, "out", data=bytes.fromhex(TRAILER_DATA))]
             + [Step(POLL, "in", status=2)] * 10)
    other = FakeTransport([Step(INQUIRY, "in", response=MICRO_INQ),
                           Step(POLL, "in", response=b"\x00\x00"),
                           Step(INQUIRY, "in", response=MICRO_INQ)])
    stale = FakeTransport([Step(INQUIRY, "in", response=LOADER_INQ)])
    opened = {"/dev/sg1": stale, "/dev/sg4": other}
    t, (path, inq), _ = _run(steps, nodes=lambda: ["/dev/sg1", "/dev/sg4"],
                             opener=opened.__getitem__)
    assert path == "/dev/sg4" and inq == MICRO_INQ
    assert t._pos == len(t._steps) and other._pos == 3 and stale._pos == 1


def test_poll_timeout():
    steps = ([Step(INQUIRY, "in", response=LOADER_INQ),
              Step(D800, "in", response=bytes(0x42))]
             + _chunks()
             + [Step(TRAILER_CDB, "out", data=bytes.fromhex(TRAILER_DATA))]
             + [Step(POLL, "in", response=b"\x04\x00")] * 5)
    clock = iter([0.0] + [float(i * 20) for i in range(1, 50)])
    with pytest.raises(up.UploadError, match="timed out"):
        _run(steps, clock=lambda: next(clock))


def _script(tmp_path, steps):
    p = tmp_path / "s.json"
    p.write_text(json.dumps({"steps": [step_to_dict(s) for s in steps]}))
    return p


def test_cli_upload_happy(tmp_path, capsys, monkeypatch):
    monkeypatch.delenv("FLEXTIGHT_DEVICE", raising=False)
    monkeypatch.delenv("FLEXTIGHT_SCRIPT", raising=False)
    fw = tmp_path / "MICROCOD.3XX"
    fw.write_bytes(FW)
    s = _script(tmp_path, _happy_steps())
    assert main(["--script", str(s), "upload-firmware", "--firmware", str(fw)]) == 0
    out = capsys.readouterr().out
    assert "FlexTight II" in out and "upload complete" in out


def test_cli_upload_refuses_microcode(tmp_path, capsys, monkeypatch):
    monkeypatch.delenv("FLEXTIGHT_DEVICE", raising=False)
    monkeypatch.delenv("FLEXTIGHT_SCRIPT", raising=False)
    fw = tmp_path / "MICROCOD.3XX"
    fw.write_bytes(FW)
    s = _script(tmp_path, [Step(INQUIRY, "in", response=MICRO_INQ)])
    assert main(["--script", str(s), "upload-firmware", "--firmware", str(fw)]) == 1
    assert "not the loader" in capsys.readouterr().err


def test_cli_upload_missing_firmware(tmp_path, capsys, monkeypatch):
    monkeypatch.delenv("FLEXTIGHT_DEVICE", raising=False)
    monkeypatch.delenv("FLEXTIGHT_SCRIPT", raising=False)
    s = _script(tmp_path, [])
    assert main(["--script", str(s), "upload-firmware",
                 "--firmware", str(tmp_path / "nope")]) == 1
    assert "cannot read firmware" in capsys.readouterr().err
