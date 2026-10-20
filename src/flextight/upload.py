# SPDX-License-Identifier: GPL-2.0-or-later
"""MICROCOD.3XX upload to the resident SCSI Loader.

A port of libft/ft_upload.c and ftpoc.c upload_microcode/await_microcode
(hardware-proven on Windows; notes/protocol.md "Startup (cold)"):

1. INQUIRY; refuse unless the product field says "SCSI Loader".
2. ReadBuffer of the D800 page (0x42 B), as FlexColor does right before
   the chunks; a failure is only reported.
3. 16 x WRITE BUFFER ``3b 01 00 00 00 00 00 10 0a 00``: data = header
   ``01 (0x30+i) 00*8`` (write memory to 0x3i00:0000) + 4096 code bytes.
   Any failure aborts; the run trailer is never sent after a partial load.
4. WRITE BUFFER trailer ``3b 01 00 00 00 00 00 00 0a 00``, data
   ``00 30 01 00 0e 00*5`` (run from 3001:000E).
5. Poll ScannerReady (``df``, 2 B) until ``00 00`` (``04 00`` busy ~6 s).
6. INQUIRY must now name the microcode (Imacon, not the loader), possibly
   on another /dev/sg node: probe them all for up to 30 s.

Only these captured commands are ever sent.
"""

from __future__ import annotations

import glob
import time

from . import protocol
from .transport import SGIOTransport, TransportError

CHUNK = 4096
CHUNKS = 16
FIRMWARE_SIZE = CHUNK * CHUNKS
HDR = 10
DEFAULT_FIRMWARE = "MICROCOD.3XX"

# ReadBuffer, buffer d8, offset 0, 0x42 bytes (ftpoc upload_microcode)
CDB_READ_D800 = bytes([0x3C, 0, 0xD8, 0, 0, 0, 0, 0, 0x42, 0])
D800_LEN = 0x42
# ScannerReady: pure 2-byte data-in poll (ftpoc CDB_POLL)
CDB_POLL = bytes([0xDF, 0, 0, 0, 0x02, 0])

POLL_TIMEOUT_S = 60.0
POLL_INTERVAL_S = 0.2
POLL_FAILS_BEFORE_PROBE = 10
AWAIT_TIMEOUT_S = 30.0
AWAIT_INTERVAL_S = 1.0


class UploadError(Exception):
    """The upload was refused or failed; the message says what to do."""


def _write_buffer_cdb(length: int) -> bytes:
    return bytes([0x3B, 0x01, 0, 0, 0, 0,
                  (length >> 16) & 0xFF, (length >> 8) & 0xFF, length & 0xFF, 0])


def chunk_packet(i: int, code: bytes) -> tuple[bytes, bytes]:
    """(cdb, data) of chunk i (0..15), as ft_upload_chunk builds them."""
    if not 0 <= i < CHUNKS:
        raise ValueError(f"upload: chunk {i} (0..15)")
    if len(code) != CHUNK:
        raise ValueError(f"upload: chunk {i} is {len(code)} bytes, not {CHUNK}")
    hdr = bytes([0x01, 0x30 + i]) + bytes(HDR - 2)
    data = hdr + bytes(code)
    return _write_buffer_cdb(len(data)), data


def trailer_packet() -> tuple[bytes, bytes]:
    """(cdb, data) of the run trailer, as ft_upload_trailer builds them."""
    data = bytes([0x00, 0x30, 0x01, 0x00, 0x0E, 0, 0, 0, 0, 0])
    return _write_buffer_cdb(len(data)), data


def is_loader(inq: bytes) -> bool:
    """ft_upload_is_loader: "SCSI Loader" at product byte 16."""
    return len(inq) >= 27 and inq[16:27] == b"SCSI Loader"


def is_microcode(inq: bytes) -> bool:
    """Imacon vendor and not the loader (ftpoc await_microcode)."""
    return len(inq) >= 27 and inq[8:14] == b"Imacon" and not is_loader(inq)


def describe(inq: bytes) -> str:
    def f(a, b):
        return inq[a:b].decode("ascii", "replace").strip()
    return f"{f(8, 16)} {f(16, 32)} {f(32, 36)}".strip()


def _inquiry(t) -> bytes:
    return t.cmd_in(protocol.CDB_INQUIRY(), protocol.INQUIRY_ALLOC_LEN, allow_short=True)


def sg_nodes() -> list[str]:
    return sorted(glob.glob("/dev/sg*"), key=lambda p: (len(p), p))


def probe_microcode(nodes, opener, log):
    """First node whose INQUIRY names the microcode: (path, transport, inq),
    the transport left open; or None.  Only INQUIRY is sent."""
    for path in nodes():
        try:
            t = opener(path)
        except TransportError:
            continue
        keep = False
        try:
            inq = _inquiry(t)
            log(f"  {path}: {describe(inq)}")
            if is_microcode(inq):
                keep = True
                return path, t, inq
        except TransportError as e:
            log(f"  {path}: no INQUIRY answer ({e})")
        finally:
            if not keep:
                try:
                    t.close()
                except TransportError:
                    pass
    return None


def upload(transport, firmware: bytes, *, device: str | None = None,
           nodes=sg_nodes, opener=SGIOTransport.open, log=print,
           sleep=time.sleep, clock=time.monotonic) -> tuple[str | None, bytes]:
    """Upload ``firmware`` through ``transport`` (the loader's node).

    Returns (path, inquiry) of the node the microcode answers on (path is
    ``device`` when it is the same node).  Transports opened here are
    closed before returning; ``transport`` itself is the caller's.
    Raises UploadError on refusal/failure.
    """
    try:
        inq = _inquiry(transport)
    except TransportError as e:
        raise UploadError(f"INQUIRY: {e}; not uploading") from e
    if not is_loader(inq):
        raise UploadError(
            f'device is not the loader (product "{inq[16:32].decode("ascii", "replace")}"); '
            "refusing. Power-cycle first.")
    log(f"loader: {describe(inq)}")
    if len(firmware) != FIRMWARE_SIZE:
        raise UploadError(f"firmware is {len(firmware)} bytes, expected "
                          f"{FIRMWARE_SIZE} (MICROCOD.3XX)")

    try:
        transport.cmd_in(CDB_READ_D800, D800_LEN, allow_short=True)
        log(f"loader page d800 (0x{D800_LEN:x}): ok")
    except TransportError as e:
        log(f"loader page d800 (0x{D800_LEN:x}): {e}")

    for i in range(CHUNKS):
        cdb, data = chunk_packet(i, firmware[i * CHUNK:(i + 1) * CHUNK])
        try:
            transport.cmd_out(cdb, data)
        except TransportError as e:
            log(f"chunk {i:2d} (to {0x30 + i:02x}00:0000): {e}")
            raise UploadError("upload aborted (no run trailer sent); "
                              "power-cycle and retry") from e
        log(f"chunk {i:2d} (to {0x30 + i:02x}00:0000): ok")

    cdb, data = trailer_packet()
    try:
        transport.cmd_out(cdb, data)
        log("trailer: ok")
    except TransportError as e:
        # ftpoc reports and carries on polling: the run may have started
        log(f"trailer: {e}")

    extra = None  # (path, transport) opened here after a re-probe
    try:
        cur, cur_path = transport, device
        log("waiting for firmware commit (poll until idle)")
        start = clock()
        last, fails = None, 0
        while True:
            try:
                r = cur.cmd_in(CDB_POLL, 2)
                v = (r[0] << 8) | r[1]
                err = None
            except TransportError as e:
                v, err = None, str(e)
            if v == 0x0000:
                break
            ms = int((clock() - start) * 1000)
            shown = v if v is not None else err
            if shown != last:
                log(f"  {ms:5d} ms: " + (f"{v:04x}" if v is not None else f"poll {err}"))
                last = shown
            if v is None:
                fails += 1
                if fails >= POLL_FAILS_BEFORE_PROBE:
                    # the microcode may answer on another node than the loader
                    fails = 0
                    found = probe_microcode(nodes, opener, log)
                    if found:
                        path, t, _ = found
                        if extra:
                            extra[1].close()
                        extra = (path, t)
                        cur, cur_path = t, path
                        last = None
                    else:
                        log("  no microcode answering yet")
            else:
                fails = 0
            if clock() - start > POLL_TIMEOUT_S:
                raise UploadError("timed out waiting for idle; upload may have failed")
            sleep(POLL_INTERVAL_S)
        log(f"device idle after {int((clock() - start) * 1000)} ms")

        # await_microcode: this node first, then every node for 30 s
        try:
            inq = _inquiry(cur)
        except TransportError:
            inq = b""
        if is_microcode(inq):
            log(f"microcode: {cur_path or '(this node)'}: {describe(inq)}")
            return cur_path, inq
        log("the microcode does not answer on this node yet; probing")
        start = clock()
        while True:
            found = probe_microcode(nodes, opener, log)
            if found:
                path, t, inq = found
                t.close()
                log(f"microcode: {path}: {describe(inq)}")
                return path, inq
            if clock() - start > AWAIT_TIMEOUT_S:
                break
            sleep(AWAIT_INTERVAL_S)
        raise UploadError(
            "the microcode did not answer within 30 s of the upload (only the "
            "loader, or nothing). Power the scanner off (>= 10 s) and on, then retry.")
    finally:
        if extra:
            try:
                extra[1].close()
            except TransportError:
                pass
