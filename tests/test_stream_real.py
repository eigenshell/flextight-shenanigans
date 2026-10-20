# SPDX-License-Identifier: GPL-2.0-or-later
"""The streaming frame detector (libft/ft_stream_detect) on real previews
from the box, line by line as they arrived, against the box's own
whole-strip detection of the same preview (strip.preview.job, ft_detect):
the same frames, each stream box holding the box's frame and within a
small margin of it.

Real strips are scans of the user's film: never in git. Put each
preview's strip.raw, strip.raw.cal, strip.raw.txt and strip.preview.job
in a folder under FT_STREAM_DATA (default notes/data/stream, gitignored);
skipped without any."""
import os
import re
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
LIBFT = ROOT / "libft"
DATA = Path(os.environ.get("FT_STREAM_DATA", ROOT / "notes" / "data" /
                           "stream"))
STRIPS = sorted(p for p in DATA.glob("*")
                if (p / "strip.raw").exists() and
                (p / "strip.preview.job").exists()) if DATA.is_dir() else []
# the stream box's edges against the box's: no tilt correction, the box
# padded by FT_SDET_PAD_MM (0.3) each side; never inside the box's frame
# by more than INSIDE (nothing cropped away), nor outside it by more than
# MARGIN (2026-10-18: 0.25..0.45 mm on 31 of 32 edges of three strips;
# 1.17 mm where a leader with faint content runs into frame 1)
INSIDE, MARGIN = 0.15, 1.25
PAD, YM = 0.3, 25.4 / 525


def box_frames(text):
    """[(type, y0, y1, x0, x1)] of a job's frames"""
    out = []
    for sec in re.split(r"^\[frame \d+\]\n", text, flags=re.M)[1:]:
        t = re.search(r"^type = (\w+)", sec, re.M).group(1)
        y0, x0, ln, w = map(float, re.search(
            r"^window = (\S+) (\S+) (\S+) (\S+)", sec, re.M).groups())
        out.append((t, y0, y0 + ln, x0, x0 + w))
    return out


def stream(strip):
    exe = LIBFT / "test" / "stream_dump"
    r = subprocess.run(["make", "-s", "-C", str(LIBFT), "test/stream_dump"],
                       capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    film = "slide" if re.search(r"^film = slide", (
        strip / "strip.preview.job").read_text(), re.M) else "neg"
    r = subprocess.run([str(exe), str(strip / "strip"), film],
                       capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    fr = []
    for line in r.stdout.splitlines():
        w = line.split()
        if w[0] == "frame":
            fr.append((w[2], *map(float, w[3:7]), int(w[7]), w[8]))
    return fr, r.stdout


@pytest.mark.skipif(not STRIPS or shutil.which("make") is None,
                    reason="no real previews in FT_STREAM_DATA "
                           "(notes/data/stream)")
@pytest.mark.parametrize("strip", STRIPS, ids=[p.name for p in STRIPS])
def test_stream_finds_the_box_frames(strip):
    want = box_frames((strip / "strip.preview.job").read_text())
    got, out = stream(strip)
    assert [g[0] for g in got] == [w[0] for w in want], out
    for g, w in zip(got, want):
        for k, sign in ((1, -1), (2, 1), (3, -1), (4, 1)):
            d = (g[k] - w[k]) * sign      # > 0: the stream box is larger
            assert -INSIDE <= d <= MARGIN, (strip.name, g, w, out)
        assert not g[5], out              # gaps found: no grid frame
    # each frame shows while the preview is still coming: sealed by the
    # lines of the gap after it (FT_SDET_GAP_MM, 0.4 mm) or the film's end
    # just after it, within 1 mm of lines; never by the strip's end
    for g in got:
        assert g[6] != "end", out
        assert int(g[6]) * YM <= g[2] - PAD + 1.0, (g, out)


@pytest.mark.skipif(not STRIPS or shutil.which("make") is None,
                    reason="no real previews in FT_STREAM_DATA "
                           "(notes/data/stream)")
@pytest.mark.parametrize("strip", STRIPS, ids=[p.name for p in STRIPS])
def test_engine_streams_the_same_frames(strip):
    """the page's engine (ftweb_cli --stream: raw lines in random chunks,
    calibrated line by line) finds stream_dump's frames to the 0.001 mm"""
    r = subprocess.run(["make", "-s", "-C", str(ROOT / "tools" / "ftweb"),
                        "ftweb_cli"], capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    want, _ = stream(strip)
    r = subprocess.run([str(ROOT / "tools" / "ftweb" / "ftweb_cli"),
                        "--stream", str(strip / "strip"), "--chunk", "0"],
                       capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    got = [tuple(map(float, w)) for w in re.findall(
        r"window = (\S+) (\S+) (\S+) (\S+)", r.stdout)]
    assert len(got) == len(want)
    for (y0, x0, ln, wd), w in zip(got, want):
        assert (y0, x0) == (w[1], w[3])
        assert abs(y0 + ln - w[2]) < 0.0015 and abs(x0 + wd - w[4]) < 0.0015
