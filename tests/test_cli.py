# SPDX-License-Identifier: GPL-2.0-or-later
"""CLI tests: in-process main() with monkeypatched env, plus one subprocess smoke."""

import json
import os
import glob
import subprocess
import sys
from pathlib import Path

import pytest

from flextight.cli import main

FIXTURE = Path(__file__).parent / "fixtures" / "info_script.json"

STUB_MSG = "not implemented: requires protocol spec (Task 10, hardware-gated)"


@pytest.fixture(autouse=True)
def _clean_env(monkeypatch):
    monkeypatch.delenv("FLEXTIGHT_DEVICE", raising=False)
    monkeypatch.delenv("FLEXTIGHT_SCRIPT", raising=False)


def test_info_via_env(monkeypatch, capsys):
    monkeypatch.setenv("FLEXTIGHT_SCRIPT", str(FIXTURE))
    assert main(["info"]) == 0
    out = capsys.readouterr().out
    assert "IMACON" in out
    assert "FlexTight" in out
    assert "6" in out


def test_info_script_flag_overrides_env(monkeypatch, capsys, tmp_path):
    bad = tmp_path / "bad.json"
    bad.write_text(json.dumps({"steps": [{"cdb": "120000002400", "direction": "in"}]}))
    monkeypatch.setenv("FLEXTIGHT_SCRIPT", str(bad))
    assert main(["--script", str(FIXTURE), "info"]) == 0
    assert "IMACON" in capsys.readouterr().out


def test_info_script_flag_after_subcommand(capsys):
    assert main(["info", "--script", str(FIXTURE)]) == 0
    assert "IMACON" in capsys.readouterr().out


def test_flag_after_subcommand_overrides_bad_env(monkeypatch, capsys, tmp_path):
    # Env points at a script whose first step mismatches; the flag AFTER the
    # subcommand must still win (pins the SUPPRESS override in the subtle
    # position, not just the flag-first one).
    bad = tmp_path / "bad.json"
    bad.write_text(json.dumps({"steps": [{"cdb": "120000002400", "direction": "in"}]}))
    monkeypatch.setenv("FLEXTIGHT_SCRIPT", str(bad))
    assert main(["info", "--script", str(FIXTURE)]) == 0
    assert "IMACON" in capsys.readouterr().out


def test_flag_in_both_positions_later_wins(monkeypatch, capsys, tmp_path):
    # Same flag in both positions with different values: the one AFTER the
    # subcommand wins, because the subparser parses last into the same
    # namespace. Pinned deliberately -- argparse behavior, not policy.
    bad = tmp_path / "bad.json"
    bad.write_text(json.dumps({"steps": [{"cdb": "120000002400", "direction": "in"}]}))
    assert main(["--script", str(bad), "info", "--script", str(FIXTURE)]) == 0
    assert "IMACON" in capsys.readouterr().out
    assert main(["--script", str(FIXTURE), "info", "--script", str(bad)]) == 1


def test_no_source_configured(capsys):
    assert main(["info"]) == 2
    err = capsys.readouterr().err
    assert "device" in err
    assert "script" in err


def test_missing_script_file(capsys, tmp_path):
    assert main(["--script", str(tmp_path / "nope.json"), "info"]) == 2
    assert capsys.readouterr().err.strip()


def test_missing_steps_key(capsys, tmp_path):
    script = tmp_path / "no_steps.json"
    script.write_text(json.dumps({"not_steps": []}))
    assert main(["--script", str(script), "info"]) == 2
    assert "missing 'steps' key" in capsys.readouterr().err


@pytest.mark.parametrize(
    "envelope",
    [
        {"steps": {"cdb": "00"}},  # steps not a list
        {"steps": ["foo"]},  # non-dict step element
    ],
)
def test_malformed_step_envelope(capsys, tmp_path, envelope):
    script = tmp_path / "bad_envelope.json"
    script.write_text(json.dumps(envelope))
    assert main(["--script", str(script), "info"]) == 2
    err = capsys.readouterr().err
    assert "Traceback" not in err
    if isinstance(envelope["steps"], list):
        assert "step 0" in err


def test_bad_direction_in_script(capsys, tmp_path):
    script = tmp_path / "bad_dir.json"
    script.write_text(
        json.dumps({"steps": [{"cdb": "000000000000", "direction": "sideways"}]})
    )
    assert main(["--script", str(script), "info"]) == 2
    err = capsys.readouterr().err
    assert "step 0" in err
    assert "direction" in err


def test_invalid_hex_in_script(capsys, tmp_path):
    script = tmp_path / "bad_hex.json"
    script.write_text(
        json.dumps({"steps": [{"cdb": "zzzz", "direction": "none"}]})
    )
    assert main(["--script", str(script), "info"]) == 2
    err = capsys.readouterr().err
    assert "step 0" in err
    assert "cdb" in err


def test_empty_steps_exits_1(monkeypatch, capsys, tmp_path):
    script = tmp_path / "empty.json"
    script.write_text(json.dumps({"steps": []}))
    monkeypatch.setenv("FLEXTIGHT_SCRIPT", str(script))
    assert main(["info"]) == 1
    assert "script length 0" in capsys.readouterr().err


def test_ls(capsys):
    assert main(["ls"]) == 0
    out, err = capsys.readouterr()
    if not glob.glob("/dev/sg*"):
        # Nothing on stdout keeps `for dev in $(flextight ls)` safe; the
        # note goes to stderr.
        assert out == ""
        assert "no /dev/sg*" in err


@pytest.mark.parametrize("sub", ["preview", "scan"])
def test_stubs(sub, capsys):
    assert main([sub]) == 2
    assert STUB_MSG in capsys.readouterr().err


def test_transport_error_maps_to_exit_1(monkeypatch, capsys, tmp_path):
    # First step expects INQUIRY but enumerate() sends TEST UNIT READY first.
    script = tmp_path / "mismatch.json"
    script.write_text(
        json.dumps({"steps": [{"cdb": "120000002400", "direction": "in"}]})
    )
    monkeypatch.setenv("FLEXTIGHT_SCRIPT", str(script))
    assert main(["info"]) == 1
    assert capsys.readouterr().err.strip()


def test_broken_pipe_maps_to_130(monkeypatch, capsys):
    # Pin the 130 mapping (and the devnull redirect that keeps the
    # interpreter's shutdown flush from printing noise over the exit).
    monkeypatch.setattr("flextight.cli._cmd_ls", lambda: (_ for _ in ()).throw(BrokenPipeError))
    assert main(["ls"]) == 130
    assert "Traceback" not in capsys.readouterr().err


def test_real_pipe_epipe_subprocess(tmp_path):
    # Buffered stdout defers raw writes to interpreter shutdown, where
    # EPIPE escapes main's handler (exit 120 + "Exception ignored" noise).
    # The driver parks 4 KB in the pipe buffer (no raw write yet), then
    # main's own flush forces the write into a readerless pipe: EPIPE
    # must surface inside main's try and map to a clean 130.
    driver = tmp_path / "driver.py"
    driver.write_text(
        "import sys\n"
        "from flextight.cli import main\n"
        "sys.stdout.write('x' * 4096)  # fits the block buffer; no raw write yet\n"
        "raise SystemExit(main(['ls']))\n"
    )
    src = str(Path(__file__).parents[1] / "src")
    env = dict(os.environ, PYTHONPATH=src)
    proc = subprocess.Popen(
        [sys.executable, str(driver)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        env=env,
    )
    proc.stdout.close()  # no reader on the pipe
    _, err = proc.communicate()
    assert proc.returncode == 130
    assert b"Exception ignored" not in err


def test_main_module_subprocess_smoke():
    src = str(Path(__file__).parents[1] / "src")
    env = dict(os.environ, PYTHONPATH=src)
    result = subprocess.run(
        [sys.executable, "-m", "flextight", "ls"],
        capture_output=True,
        text=True,
        env=env,
    )
    assert result.returncode == 0
