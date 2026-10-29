# SPDX-License-Identifier: GPL-2.0-or-later
"""ftdev print (libft/ft_print.h) end to end on the synthetic share window,
and tools/cube2c.py, the emulsions' 3D-table compiler."""
import math
import shutil
import struct
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
FTDEV = ROOT / "tools" / "ftdev"
LIBFT = ROOT / "libft"
CUBE2C = ROOT / "tools" / "cube2c.py"
needs_cc = pytest.mark.skipif(
    shutil.which("make") is None or shutil.which("cc") is None,
    reason="needs make and a C compiler")


@pytest.fixture(scope="module")
def ftdev():
    r = subprocess.run(["make", "-C", str(FTDEV)], capture_output=True,
                       text=True)
    assert r.returncode == 0, r.stdout + r.stderr
    return FTDEV / "ftdev"


def run(ftdev, *args):
    return subprocess.run([str(ftdev), *map(str, args)], capture_output=True,
                          text=True)


def sidecar(path):
    return dict(l.split("=", 1) for l in path.read_text().splitlines())


@pytest.fixture
def roll(ftdev, tmp_path):
    """The synthetic share window (libft/test/window_dump.c) cut, and its
    frame copied as frames 2 and 3 (sidecars renumbered): a 3-frame roll."""
    r = subprocess.run(["make", "-C", str(LIBFT), "test/window_dump"],
                       capture_output=True, text=True)
    assert r.returncode == 0, r.stdout + r.stderr
    base = tmp_path / "job_win01"
    subprocess.run([str(LIBFT / "test" / "window_dump"), str(base)],
                   check=True)
    r = run(ftdev, "cut", str(base) + ".raw")
    assert r.returncode == 0, r.stderr
    one = tmp_path / "job_frame01.dng"
    frames = [one]
    for n in (2, 3):
        dng = tmp_path / ("job_frame%02d.dng" % n)
        shutil.copy(one, dng)
        txt = Path(str(one) + ".txt").read_text()
        Path(str(dng) + ".txt").write_text(
            txt.replace("frame=1\n", "frame=%d\n" % n))
        frames.append(dng)
    return frames


def jpeg_size(path):
    """(width, height) from the JPEG's SOF0"""
    data = path.read_bytes()
    at = data.index(b"\xff\xc0")
    h, w = struct.unpack(">HH", data[at + 5:at + 9])
    return w, h


def mean_rgb(path):
    cv2 = pytest.importorskip("cv2")
    img = cv2.imread(str(path))
    h, w = img.shape[:2]
    mid = img[h // 4:3 * h // 4, w // 4:3 * w // 4].reshape(-1, 3)
    b, g, r = mid.mean(axis=0)
    return r, g, b


@needs_cc
def test_emulsions_lists_the_table(ftdev):
    r = run(ftdev, "emulsions")
    assert r.returncode == 0, r.stderr
    assert r.stdout.splitlines() == [
        "emulsions (science revision 1):",
        "  generic-neg      negative  Generic colour negative",
        "  generic-slide    slide     Generic slide",
        "  generic-bw       b&w       Generic black and white",
    ]


@needs_cc
def test_print_one_frame(ftdev, roll):
    dng = roll[0]
    r = run(ftdev, "print", dng, "--emulsion", "generic-neg")
    assert r.returncode == 0, r.stderr
    assert "1 frame: print a roll's frames together" in r.stderr
    out = dng.with_suffix(".jpg")
    rec = sidecar(Path(str(out) + ".txt"))
    assert rec["emulsion"] == "generic-neg" and rec["science"] == "1"
    assert rec["chain"] == "stock frontier classic classic"
    assert rec["frame"] == "1" and rec["cmyd"] == "0 0 0 0"
    assert rec["film"] == "neg" and rec["base"] == "0.2076 0.3979 0.6021"
    assert rec["range"].split()[1] == "1.2"            # the stock's scale
    assert rec["exposure"] == rec["auto_exposure"]
    assert rec["tint"] == rec["auto_tint"]
    assert (rec["nr_sensor"], rec["nr_chroma"], rec["nr_grain"],
            rec["sharpen"]) == ("1", "2", "0", "0.5")
    assert rec["space"] == "srgb" and rec["gamut"] == "soft"
    # the share size, and the same text in the JPEG's comment
    assert max(jpeg_size(out)) == 2000
    text = Path(str(out) + ".txt").read_bytes()
    data = out.read_bytes()
    at = data.index(b"\xff\xfe")
    assert data[at + 4:at + 4 + len(text)] == text


@needs_cc
def test_print_clicks(ftdev, roll, tmp_path):
    dng = roll[0]

    def one(name, cmyd):
        out = tmp_path / name
        r = run(ftdev, "print", dng, "--emulsion", "generic-neg", "--cmyd",
                cmyd, "-o", out)
        assert r.returncode == 0, r.stderr
        return out, sidecar(Path(str(out) + ".txt"))

    a, ra = one("a.jpg", "0,0,0,0")
    d, rd = one("d.jpg", "0,0,0,6")
    c, rc = one("c.jpg", "6,0,0,0")
    assert rd["cmyd"] == "0 0 0 6" and rc["cmyd"] == "6 0 0 0"
    # +6 D: 0.15 D less print exposure; +6 C: 15 CC less red
    assert abs(float(rd["exposure"]) - float(ra["exposure"]) +
               6 * 0.025 / math.log10(2)) < 1e-4
    assert abs(float(rc["tint"].split()[0]) -
               float(ra["tint"].split()[0]) + 15) < 1e-4
    ma, md, mc = mean_rgb(a), mean_rgb(d), mean_rgb(c)
    assert all(md[k] < ma[k] - 3 for k in range(3))   # darker
    assert mc[0] < ma[0] - 3                          # less red: cyan
    assert abs(mc[1] - ma[1]) < abs(mc[0] - ma[0])


@needs_cc
def test_print_roll_file(ftdev, roll, tmp_path):
    pf = tmp_path / "job.print"
    pf.write_text("emulsion = generic-neg\n[frame 2]\ncmyd = 0 0 0 4\n"
                  "[frame 9]\ncmyd = 1 0 0 0\n")
    r = run(ftdev, "print", *roll, "--roll", pf)
    assert r.returncode == 0, r.stderr
    assert "warning: %s: [frame 9] is not in the job: ignored" % pf \
        in r.stderr
    recs = [sidecar(Path(str(d.with_suffix(".jpg")) + ".txt")) for d in roll]
    assert [x["frame"] for x in recs] == ["1", "2", "3"]
    assert [x["cmyd"] for x in recs] == ["0 0 0 0", "0 0 0 4", "0 0 0 0"]
    # the same pixels on one roll: the same auto
    assert recs[0]["auto_exposure"] == recs[2]["auto_exposure"]
    assert recs[0]["auto_tint"] == recs[2]["auto_tint"]
    # --job: the job's frames decide what is "in the job"
    job = tmp_path / "s.job"
    job.write_text(
        "[strip]\nholder = 24x217\nfilm = negative\nfocus = shifted\n"
        "focus_offset = 0\n" + "".join(
            "\n[frame %d]\ntype = full\ninclude = yes\nppi = 3150\n"
            "bin = 1\nwindow = %.2f 1.31 2.0 0.8\n" % (n, 40 + 40 * n)
            for n in (1, 2, 9)))
    r = run(ftdev, "print", *roll, "--roll", pf, "--job", job)
    assert r.returncode == 0, r.stderr
    assert "not in the job" not in r.stderr


@needs_cc
def test_print_bw(ftdev, roll, tmp_path):
    out = tmp_path / "bw.jpg"
    r = run(ftdev, "print", roll[0], "--emulsion", "generic-bw", "--cmyd",
            "0,0,0,2", "-o", out)
    assert r.returncode == 0, r.stderr
    rec = sidecar(Path(str(out) + ".txt"))
    assert rec["film"] == "bw" and rec["tint"] == "0 0 0"
    rr, gg, bb = mean_rgb(out)
    assert abs(rr - gg) < 1.5 and abs(gg - bb) < 1.5


@needs_cc
def test_print_refusals(ftdev, roll, tmp_path):
    dng = roll[0]

    def refused(*args, msg):
        r = run(ftdev, "print", *args)
        assert r.returncode == 2, r.stderr
        assert msg in r.stderr, r.stderr

    refused(dng, msg="print needs --emulsion <id> or --roll <roll.print>")
    refused(dng, "--emulsion", "portra400",
            msg="--emulsion: 'portra400' is not an emulsion (generic-neg "
                "generic-slide generic-bw)")
    pf = tmp_path / "r.print"
    pf.write_text("emulsion = generic-neg\n")
    refused(dng, "--roll", pf, "--emulsion", "generic-neg",
            msg="with --roll the emulsion and the clicks come from the "
                ".print file")
    refused(dng, "--emulsion", "generic-neg", "--job", "x.job",
            msg="--job needs --roll")
    refused(*roll[:2], "--emulsion", "generic-neg", "-o", tmp_path / "x.jpg",
            msg="-o needs exactly one frame")
    for bad in ("1,2,3", "0,0,0,100", "a,b,c,d"):
        refused(dng, "--emulsion", "generic-neg", "--cmyd", bad,
                msg="--cmyd is c,m,y,d (whole numbers, -99..99; b&w: "
                    "0,0,0,d)")
    refused(dng, "--emulsion", "generic-bw", "--cmyd", "1,0,0,0",
            msg="--cmyd is c,m,y,d")
    refused(dng, "--emulsion", "generic-slide",
            msg="%s is a negative; generic-slide prints slides" % dng)
    pf.write_text("emulsion = generic-neg\n[frame 2]\ncmyd = 1 2\n")
    refused(dng, "--roll", pf,
            msg="%s: line 3: cmyd: '1 2' is not four whole numbers" % pf)
    bare = tmp_path / "bare.dng"
    shutil.copy(dng, bare)
    pf.write_text("emulsion = generic-neg\n")
    refused(bare, "--roll", pf,
            msg="%s: no frame number (no sidecar, no _frameNN in the name)"
                % bare)
    refused(msg="usage: ftdev print")


def test_cube2c_header_in_sync(tmp_path):
    out = tmp_path / "luts.h"
    subprocess.run([sys.executable, str(CUBE2C), str(LIBFT / "emulsions"),
                    str(out)], check=True)
    assert out.read_bytes() == (LIBFT / "ft_emulsion_luts.h").read_bytes(), \
        "libft/ft_emulsion_luts.h is stale: run make luts in libft/"


@needs_cc
def test_cube2c_compiles_a_table(tmp_path):
    src = tmp_path / "cubes"
    src.mkdir()
    rows = ["%g %g %g" % (r * 0.5, g * 0.25, b) for b in (0, 1)
            for g in (0, 1) for r in (0, 1)]
    (src / "Test-Look.cube").write_text(
        'TITLE "test"\n# a comment\nLUT_3D_SIZE 2\nDOMAIN_MIN 0 0 0\n'
        "DOMAIN_MAX 1 1 1\n" + "\n".join(rows) + "\n")
    out = tmp_path / "ft_emulsion_luts.h"
    subprocess.run([sys.executable, str(CUBE2C), str(src), str(out)],
                   check=True)
    text = out.read_text()
    assert '/* Test-Look.cube: "test", 2^3 */' in text
    assert "static const ft_cube CUBE_test_look = { 2, " \
           "CUBE_test_look_RGB };" in text
    prog = tmp_path / "t.c"
    prog.write_text(
        '#include <stdio.h>\n#include "ft_emulsion_luts.h"\n'
        "int main(void) { printf(\"%d %g %g\\n\", CUBE_test_look.size, "
        "(double)CUBE_test_look.nodes[3], (double)CUBE_test_look.nodes[22]);"
        " return 0; }\n")
    exe = tmp_path / "t"
    r = subprocess.run(["cc", "-std=c99", "-Wall", "-Wextra", "-Wpedantic",
                        "-Werror", "-I", str(tmp_path), "-I", str(LIBFT),
                        "-o", str(exe), str(prog)], capture_output=True,
                       text=True)
    assert r.returncode == 0, r.stderr
    r = subprocess.run([str(exe)], capture_output=True, text=True)
    assert r.stdout == "2 0.5 0.25\n"   # node (1,0,0) red; (1,1,1) green


@pytest.mark.parametrize("body, msg", [
    ("LUT_3D_SIZE 2\n0 0 0\n", "x.cube:0: 1 rows, LUT_3D_SIZE 2 needs 8"),
    ("LUT_3D_SIZE 40\n", "x.cube:1: LUT_3D_SIZE 40 (2..33)"),
    ("LUT_1D_SIZE 2\n", "x.cube:1: LUT_1D_SIZE is not supported"),
    ("DOMAIN_MAX 2 2 2\n", "x.cube:1: DOMAIN_MAX must be 1 1 1"),
    ("LUT_3D_SIZE 2\n0 0 1.5\n", "x.cube:2: values are 0..1"),
    ("LUT_3D_SIZE 2\n0 0\n", "x.cube:2: not three numbers"),
    ("0 0 0\n", "x.cube:0: no LUT_3D_SIZE"),
])
def test_cube2c_refuses(tmp_path, body, msg):
    src = tmp_path / "cubes"
    src.mkdir()
    (src / "x.cube").write_text(body)
    r = subprocess.run([sys.executable, str(CUBE2C), str(src),
                        str(tmp_path / "o.h")], capture_output=True,
                       text=True)
    assert r.returncode != 0
    assert msg in r.stderr, r.stderr
