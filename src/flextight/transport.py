# SPDX-License-Identifier: GPL-2.0-or-later
"""SCSI transport layer for talking to the scanner.

Two implementations share a small command interface:

- :class:`SGIOTransport` issues real ``SG_IO`` ioctls against an open
  ``/dev/sgX`` file descriptor.  The v3 ``sg_io_hdr`` struct below is
  mirrored field-for-field from ``/usr/include/scsi/sg.h``; on x86-64 it
  is 88 bytes (the 32-bit layout is 60), and the kernel parses it by its
  own C layout, so any drift here silently corrupts every pointer and
  status field.

- :class:`FakeTransport` replays an explicit script of :class:`Step`
  expectations so all protocol code can be developed and tested without
  hardware.  Trace-derived loaders are deferred until the IOCTL payload
  layout is pinned by hardware captures.
"""

from __future__ import annotations

import ctypes
import fcntl
import os
from dataclasses import dataclass

SG_IO = 0x2285

SG_DXFER_NONE = -1
SG_DXFER_TO_DEV = -2
SG_DXFER_FROM_DEV = -3

_DIRECTIONS = {"from": SG_DXFER_FROM_DEV, "to": SG_DXFER_TO_DEV, "none": SG_DXFER_NONE}

_SENSE_LEN = 64
_TIMEOUT_MS = 20000  # 20 s -- scans are slow


class TransportError(Exception):
    """Raised for ioctl failures, SCSI status errors, and script violations."""


class _SgIoHdr(ctypes.Structure):
    """``struct sg_io_hdr`` (v3) from /usr/include/scsi/sg.h, x86-64 layout."""

    _fields_ = [
        ("interface_id", ctypes.c_int),
        ("dxfer_direction", ctypes.c_int),
        ("cmd_len", ctypes.c_ubyte),
        ("mx_sb_len", ctypes.c_ubyte),
        ("iovec_count", ctypes.c_ushort),
        ("dxfer_len", ctypes.c_uint),
        ("dxferp", ctypes.c_void_p),
        ("cmdp", ctypes.c_void_p),
        ("sbp", ctypes.c_void_p),
        ("timeout", ctypes.c_uint),
        ("flags", ctypes.c_uint),
        ("pack_id", ctypes.c_int),
        ("usr_ptr", ctypes.c_void_p),
        ("status", ctypes.c_ubyte),
        ("masked_status", ctypes.c_ubyte),
        ("msg_status", ctypes.c_ubyte),
        ("sb_len_wr", ctypes.c_ubyte),
        ("host_status", ctypes.c_ushort),
        ("driver_status", ctypes.c_ushort),
        ("resid", ctypes.c_int),
        ("duration", ctypes.c_uint),
        ("info", ctypes.c_uint),
    ]


def _norm_cdb(cdb: bytes | str) -> bytes:
    return bytes.fromhex(cdb) if isinstance(cdb, str) else cdb


class SGIOTransport:
    """Real SG_IO ioctl transport over an open /dev/sgX file descriptor."""

    def __init__(self, fd: int):
        self.fd = fd
        self._bufs: tuple = ()
        self._closed = False

    @classmethod
    def open(cls, path: str) -> "SGIOTransport":
        try:
            fd = os.open(path, os.O_RDWR)
        except OSError as e:
            raise TransportError(f"cannot open {path}: {e}") from e
        return cls(fd)

    def close(self):
        """Close the device fd; a second call is a no-op.

        A failed close leaves the transport open so the caller can retry;
        only a successful os.close sets _closed.
        """
        if self._closed:
            return
        try:
            os.close(self.fd)
        except OSError as e:
            raise TransportError(f"cannot close fd {self.fd}: {e}") from e
        self._closed = True

    def _check_open(self):
        if self._closed:
            raise TransportError("transport closed")

    def _build_header(self, cdb: bytes, dxfer_len: int, dxfer: str) -> _SgIoHdr:
        cdb_buf = ctypes.create_string_buffer(cdb.ljust(16, b"\0"), 16)
        data_buf = ctypes.create_string_buffer(max(dxfer_len, 1))
        sense_buf = ctypes.create_string_buffer(_SENSE_LEN)
        self._bufs = (cdb_buf, data_buf, sense_buf)

        hdr = _SgIoHdr()
        hdr.interface_id = ord("S")
        hdr.dxfer_direction = _DIRECTIONS[dxfer]
        hdr.cmd_len = len(cdb)
        hdr.mx_sb_len = _SENSE_LEN
        hdr.dxfer_len = dxfer_len
        hdr.dxferp = ctypes.addressof(data_buf)
        hdr.cmdp = ctypes.addressof(cdb_buf)
        hdr.sbp = ctypes.addressof(sense_buf)
        hdr.timeout = _TIMEOUT_MS
        hdr.flags = 0
        return hdr

    def _exec(self, cdb: bytes, data: bytes | None, dxfer: str, expected_len: int,
              allow_short: bool = False):
        self._check_open()
        hdr = self._build_header(cdb, len(data) if data is not None else expected_len, dxfer)
        if data is not None and dxfer == "to":
            self._bufs[1].raw = data
        buf = bytearray(ctypes.string_at(ctypes.byref(hdr), ctypes.sizeof(hdr)))
        try:
            fcntl.ioctl(self.fd, SG_IO, buf, True)  # mutate=True: kernel writes status back
        except OSError as e:
            raise TransportError(f"SG_IO ioctl failed: errno {e.errno} ({e.strerror})") from e
        ctypes.memmove(ctypes.byref(hdr), bytes(buf), ctypes.sizeof(hdr))

        # sb_len_wr is kernel-written; fall back to the full sense buffer
        sense = self._bufs[2].raw[: hdr.sb_len_wr or _SENSE_LEN]
        if hdr.host_status or hdr.driver_status:
            raise TransportError(
                f"SG_IO host_status={hdr.host_status} driver_status={hdr.driver_status} "
                f"sense={sense.hex()}"
            )
        if hdr.status != 0:
            raise TransportError(f"SCSI status {hdr.status} sense={sense.hex()}")
        if dxfer == "from":
            if allow_short:
                # Underrun is normal for some commands (protocol.md: an
                # empty read is not an error); hand back what arrived.
                n = max(0, min(expected_len, expected_len - hdr.resid))
                return self._bufs[1].raw[:n]
            if hdr.resid != 0:
                # A short read must never silently zero-fill scan data; relax
                # per-command only if hardware captures show a legit short read.
                raise TransportError(
                    f"short read: resid={hdr.resid} of requested {expected_len}"
                )
            return self._bufs[1].raw[:expected_len]
        return None

    def cmd_in(self, cdb: bytes | str, length: int, allow_short: bool = False) -> bytes:
        """Data-in command.  Default: any resid underrun raises.  With
        ``allow_short`` only the transferred bytes are returned (may be
        fewer than ``length``, or empty)."""
        return self._exec(_norm_cdb(cdb), None, "from", length, allow_short)

    def cmd_out(self, cdb: bytes | str, data: bytes):
        if not data:
            raise TransportError("empty payload for cmd_out")
        return self._exec(_norm_cdb(cdb), data, "to", len(data))

    def cmd_none(self, cdb: bytes | str):
        return self._exec(_norm_cdb(cdb), None, "none", 0)


@dataclass
class Step:
    """One expected command in a FakeTransport script.

    ``cdb`` accepts bytes or a hex string (normalized in ``__post_init__``).
    ``response`` is the data an "in" command returns; ``status`` is the SCSI
    status byte (0 = good).  ``data`` is the expected payload of an "out"
    command; empty means "don't check the payload".
    """

    cdb: bytes
    direction: str  # "in" | "out" | "none"
    response: bytes = b""
    status: int = 0
    data: bytes = b""

    def __post_init__(self):
        if isinstance(self.cdb, str):
            self.cdb = bytes.fromhex(self.cdb)


def step_to_dict(s: Step) -> dict:
    """Serialize a Step to a JSON-safe dict (byte fields as hex strings)."""
    return {
        "cdb": s.cdb.hex(),
        "direction": s.direction,
        "response": s.response.hex(),
        "status": s.status,
        "data": s.data.hex(),
    }


def step_from_dict(d: dict) -> Step:
    """Build a Step from a ``step_to_dict`` dict, validating as we go.

    Raises TransportError naming the offending field on a bad direction
    or invalid hex.
    """
    direction = d.get("direction")
    if direction not in ("in", "out", "none"):
        raise TransportError(f"invalid direction field: {direction!r}")

    def _hex(field: str) -> bytes:
        try:
            return bytes.fromhex(d.get(field) or "")
        except ValueError as e:
            raise TransportError(f"invalid hex in {field!r} field") from e

    try:
        status = int(d.get("status", 0))
    except (TypeError, ValueError) as e:
        raise TransportError("invalid 'status' field") from e
    return Step(cdb=_hex("cdb"), direction=direction, response=_hex("response"),
                status=status, data=_hex("data"))


class FakeTransport:
    """Replays an explicit script of expected commands; no hardware needed."""

    def __init__(self, steps: list[Step]):
        self._steps = list(steps)
        self._pos = 0

    @classmethod
    def from_script(cls, steps: list[Step]) -> "FakeTransport":
        # Thin on purpose: future trace-derived loaders will be separate
        # classmethods building Step lists from pinned capture formats.
        return cls(steps)

    def _next(self, cdb_bytes: bytes, direction: str) -> Step:
        """Consume and return the next step, checking cdb, status, direction.

        Contract: ANY failure (exhaustion, cdb mismatch, non-zero status,
        direction mismatch) consumes the step -- there is no retry.  A
        caught TransportError therefore leaves the script positioned after
        the failed step, consistent with how real hardware would have
        executed it.
        """
        if self._pos >= len(self._steps):
            raise TransportError(
                f"script exhausted at command {cdb_bytes.hex()} "
                f"(script length {len(self._steps)})"
            )
        idx = self._pos
        step = self._steps[idx]
        self._pos += 1
        if step.cdb != cdb_bytes:
            raise TransportError(
                f"script mismatch at step {idx}: expected {step.cdb.hex()}, "
                f"got {cdb_bytes.hex()}"
            )
        if step.status != 0:
            raise TransportError(f"step {idx} failed with SCSI status {step.status}")
        if step.direction != direction:
            raise TransportError(
                f"script mismatch at step {idx}: expected direction "
                f"{step.direction!r}, got {direction!r}"
            )
        return step, idx

    def cmd_in(self, cdb: bytes | str, length: int, allow_short: bool = False) -> bytes:
        step, _ = self._next(_norm_cdb(cdb), "in")
        resp = step.response
        if allow_short:
            return resp[:length]
        if len(resp) < length:
            resp = resp + b"\0" * (length - len(resp))
        return resp[:length]

    def cmd_out(self, cdb: bytes | str, data: bytes):
        step, idx = self._next(_norm_cdb(cdb), "out")
        if step.data and step.data != data:
            raise TransportError(
                f"script data mismatch at step {idx}: "
                f"expected {step.data.hex()}, got {data.hex()}"
            )
        return None

    def cmd_none(self, cdb: bytes | str):
        self._next(_norm_cdb(cdb), "none")
        return None

    def close(self):
        """No-op: the fake owns no resources, but Scanner.close() delegates."""
        return None
