# SPDX-License-Identifier: GPL-2.0-or-later
"""Command-line interface for the Flextight scanner driver.

Transport source is selected globally, before the subcommand:

- ``--script PATH`` / ``$FLEXTIGHT_SCRIPT``: replay a JSON Step script
  (``{"steps": [step_to_dict(Step), ...]}``) through a FakeTransport.
- ``--device PATH`` / ``$FLEXTIGHT_DEVICE``: talk to a real ``/dev/sgX``.

Trace-derived replay is deferred until the Windows IOCTL payload layout
is pinned by hardware captures, so scripts use the explicit Step format.
"""

from __future__ import annotations

import argparse
import glob
import io
import json
import os
import sys

from .scanner import Scanner
from .upload import DEFAULT_FIRMWARE, UploadError, upload
from .transport import (
    FakeTransport,
    SGIOTransport,
    TransportError,
    step_from_dict,
)

_STUB_MSG = "not implemented: requires protocol spec (Task 10, hardware-gated)"


class _UsageError(Exception):
    """Bad invocation (unresolvable/malformed source): exit 2."""


def _build_parser() -> argparse.ArgumentParser:
    # Shared parent so --device/--script parse both before AND after the
    # subcommand name.  SUPPRESS defaults are load-bearing: _SubParsersAction
    # re-parses into a fresh namespace and copies every var back, so a plain
    # None default here would clobber a flag given before the subcommand.
    # With SUPPRESS the attribute is only set when the flag is present, and
    # both positions share dests -> one env-default resolution path.
    source = argparse.ArgumentParser(add_help=False)
    source.add_argument("--device", default=argparse.SUPPRESS,
                        help="real scanner device node (e.g. /dev/sg0)")
    source.add_argument("--script", default=argparse.SUPPRESS,
                        help="JSON Step script to replay")
    p = argparse.ArgumentParser(prog="flextight", parents=[source])
    sub = p.add_subparsers(dest="subcommand", required=True)
    sub.add_parser("ls", parents=[source], help="list /dev/sg* device nodes")
    sub.add_parser("info", parents=[source], help="enumerate the scanner (INQUIRY)")
    up = sub.add_parser("upload-firmware", parents=[source],
                        help="upload MICROCOD.3XX to the scanner's SCSI Loader")
    up.add_argument("--firmware", default=DEFAULT_FIRMWARE,
                    help=f"microcode file (default: {DEFAULT_FIRMWARE} in the current directory)")
    sub.add_parser("preview", parents=[source], help="acquire a preview scan (stub)")
    sub.add_parser("scan", parents=[source], help="acquire a scan (stub)")
    return p


def _resolve_source(args) -> tuple[str, str]:
    """Return (kind, path) for the single configured transport source."""
    device = getattr(args, "device", None) or os.environ.get("FLEXTIGHT_DEVICE")
    script = getattr(args, "script", None) or os.environ.get("FLEXTIGHT_SCRIPT")
    if device and script:
        raise _UsageError(
            "both --device/--script configured: exactly one transport "
            "source (--device PATH or --script PATH) must be given"
        )
    if script:
        return "script", script
    if device:
        return "device", device
    raise _UsageError(
        "no transport source: pass --device PATH or --script PATH "
        "(or set FLEXTIGHT_DEVICE / FLEXTIGHT_SCRIPT)"
    )


def _err(code: int, msg: str) -> int:
    print(f"flextight: {msg}", file=sys.stderr)
    return code


def _load_script(path: str):
    """Load a JSON Step script, raising _UsageError on any schema problem."""
    try:
        with open(path, "r", encoding="utf-8") as f:
            data = json.load(f)
        raw_steps = data["steps"]
        if not isinstance(raw_steps, list):
            raise _UsageError(f"{path}: 'steps' must be a list")
        steps = []
        for i, d in enumerate(raw_steps):
            if not isinstance(d, dict):
                raise _UsageError(f"{path}: step {i} is not an object")
            try:
                steps.append(step_from_dict(d))
            except TransportError as e:
                raise _UsageError(f"{path}: step {i}: {e}") from e
        return steps
    except _UsageError:
        raise
    except KeyError as e:
        raise _UsageError(f"{path}: missing 'steps' key") from e
    except (OSError, ValueError, TypeError) as e:
        raise _UsageError(f"{path}: invalid script: {e}") from e


def _make_transport(args):
    kind, path = _resolve_source(args)
    if kind == "script":
        return FakeTransport.from_script(_load_script(path))
    return SGIOTransport.open(path)


def _cmd_ls() -> int:
    nodes = sorted(glob.glob("/dev/sg*"))
    if not nodes:
        print("no /dev/sg* devices found", file=sys.stderr)
    for n in nodes:
        print(n)
    return 0


def _cmd_info(transport) -> int:
    scanner = Scanner(transport)
    try:
        info = scanner.enumerate()
    finally:
        scanner.close()
    for key in ("vendor", "model", "rev", "pdt"):
        print(f"{key}: {info[key]}")
    return 0


def _cmd_upload(transport, args) -> int:
    try:
        with open(args.firmware, "rb") as f:
            firmware = f.read()
    except OSError as e:
        return _err(1, f"cannot read firmware {args.firmware}: {e}")
    kind, path = _resolve_source(args)

    def log(msg):
        print(msg, flush=True)

    try:
        if kind == "device":
            node, _ = upload(transport, firmware, device=path, log=log)
        else:
            # a replayed script has no other nodes to probe
            node, _ = upload(transport, firmware, nodes=list, log=log)
    except UploadError as e:
        return _err(1, f"upload: {e}")
    print("upload complete. Don't upload again until a power cycle", flush=True)
    return 0


def main(argv=None) -> int:
    args = _build_parser().parse_args(argv)

    transport = None
    try:
        if args.subcommand == "ls":
            code = _cmd_ls()
        elif args.subcommand in ("preview", "scan"):
            code = _err(2, _STUB_MSG)
        else:
            transport = _make_transport(args)
            if args.subcommand == "info":
                code = _cmd_info(transport)
            elif args.subcommand == "upload-firmware":
                code = _cmd_upload(transport, args)
            else:
                raise AssertionError(f"unhandled subcommand {args.subcommand}")
        # Flush while the handler below can still see it: block-buffered
        # stdout (any pipe) defers raw writes to interpreter shutdown,
        # where EPIPE would surface as "Exception ignored" noise and a
        # 120 exit instead of our clean 130.
        sys.stdout.flush()
        return code
    except _UsageError as e:
        return _err(2, str(e))
    except TransportError as e:
        return _err(1, str(e))
    except (BrokenPipeError, KeyboardInterrupt):
        # A pipe reader (e.g. `flextight ls | head`) went away mid-flush;
        # point stdout at devnull so the interpreter's shutdown flush does
        # not print "Exception ignored" noise over our exit status.  Best
        # effort: under capture sys.stdout may have no real fd.
        try:
            devnull = os.open(os.devnull, os.O_WRONLY)
            os.dup2(devnull, sys.stdout.fileno())
        except (OSError, ValueError, io.UnsupportedOperation):
            pass
        return 130
    finally:
        if transport is not None:
            transport.close()


if __name__ == "__main__":
    raise SystemExit(main())
