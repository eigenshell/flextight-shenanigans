# SPDX-License-Identifier: GPL-2.0-or-later
"""ftdev plan: sizes per frame for a share scan (ft_plan_share_sizes), the
web page's planner from a terminal."""
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
FTDEV = ROOT / "tools" / "ftdev"
pytestmark = pytest.mark.skipif(
    shutil.which("make") is None or shutil.which("cc") is None,
    reason="needs make and a C compiler")

JOB = "[strip]\nholder = 24x217\nfilm = negative\nfocus = shifted\n" + "".join(
    "\n[frame %d]\ntype = full\nppi = 2000\nwindow = %.3f 1.600 36.000 "
    "24.400\n" % (n, 4 + 38.0 * (n - 1)) for n in range(1, 6))


@pytest.fixture(scope="module")
def ftdev():
    r = subprocess.run(["make", "-C", str(FTDEV)], capture_output=True,
                       text=True)
    assert r.returncode == 0, r.stdout + r.stderr
    return FTDEV / "ftdev"


def run(ftdev, *args):
    return subprocess.run([str(ftdev), *map(str, args)], capture_output=True,
                          text=True)


def frames(text):
    """{frame number: {key: value}} from a job file"""
    out, cur = {}, None
    for line in text.splitlines():
        line = line.split("#", 1)[0].strip()
        if line.startswith("[frame "):
            cur = out.setdefault(int(line[7:-1]), {})
        elif "=" in line and cur is not None:
            k, v = line.split("=", 1)
            cur[k.strip()] = v.strip()
    return out


def test_plan_sizes(ftdev, tmp_path):
    job = tmp_path / "strip.job"
    job.write_text(JOB)
    r = run(ftdev, "plan", job, "--size", "4mp:1-2,5", "--size", "6mp:3")
    assert r.returncode == 0, r.stderr
    text = job.read_text()
    assert "share = 2450   # scan to share" in text
    f = frames(text)
    assert [f[n]["include"] for n in range(1, 6)] == \
        ["yes", "yes", "yes", "no", "yes"]
    assert f[3]["share_px"] == "3000"
    assert all("share_px" not in f[n] for n in (1, 2, 4, 5))
    g = [int(f[n]["group"]) for n in (1, 2, 3, 5)]
    assert g == sorted(g) and g[2] > g[1] and g[3] > g[2]
    assert f[4]["group"] == "0"
    # the summary names every window with its size, ppi and bin
    assert "window %d: frame 3, 3000 px, %s ppi, bin %s\n" % (
        g[2], f[3]["ppi"], f[3]["bin"]) in r.stdout
    assert r.stdout.endswith(
        "%d window(s); next: ftpoc scan --job %s\n" % (g[3], job))


def test_plan_output_file_and_px(ftdev, tmp_path):
    job = tmp_path / "strip.job"
    job.write_text(JOB)
    out = tmp_path / "other.job"
    r = run(ftdev, "plan", job, "--size", "2000:1-5", "-o", out)
    assert r.returncode == 0, r.stderr
    assert job.read_text() == JOB                 # untouched
    assert "share = 2000" in out.read_text()
    assert "share_px" not in out.read_text()


@pytest.mark.parametrize("args, msg", [
    ([], "plan: no --size\n"),
    (["--size", "8mp:1"], "plan: bad size '8mp' (4mp, 6mp or px)\n"),
    (["--size", "4mp"], "plan: --size wants size:frames, e.g. 4mp:1-3,5\n"),
    (["--size", "4mp:9"], "plan: share: the job has no frame 9\n"),
    (["--size", "4mp:1-2", "--size", "6mp:2"],
     "plan: frame 2 has two sizes\n"),
    (["--size", "50:1"],
     "plan: share: frame 1: 50 px is out of range (100..20000)\n"),
])
def test_plan_refusals(ftdev, tmp_path, args, msg):
    job = tmp_path / "strip.job"
    job.write_text(JOB)
    r = run(ftdev, "plan", job, *args)
    assert r.returncode != 0
    assert r.stderr.endswith(msg)
    assert job.read_text() == JOB
