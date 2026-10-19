# SPDX-License-Identifier: GPL-2.0-or-later
"""The web page's engine (tools/ftweb) makes the CLI's JPEGs: ftweb_cli
(the engine driven from files) against ftdev cut + ftdev print --roll on
synthetic window scans; and the engine compiled to wasm32 makes the same
bytes as native (skipped without the wasi-sdk or node)."""
import json
import os
import shutil
import subprocess
import zipfile
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
LIBFT = ROOT / "libft"
FTDEV = ROOT / "tools" / "ftdev"
FTWEB = ROOT / "tools" / "ftweb"
needs_cc = pytest.mark.skipif(
    shutil.which("make") is None or shutil.which("cc") is None,
    reason="needs make and a C compiler")

# frame 2 is turned: every byte check below covers the turn
PRINT = "emulsion = generic-neg\n\n[frame 2]\ncmyd = 3 -2 0 1\n" \
        "rotate = 90\n\n[frame 9]\ncmyd = 0 0 0 2\n"


def make(*args):
    r = subprocess.run(["make", "-s", *map(str, args)], capture_output=True,
                       text=True)
    assert r.returncode == 0, r.stdout + r.stderr


def jpeg_size(b):
    """(w, h) from the SOF0 marker"""
    i = b.index(b"\xff\xc0")
    return int.from_bytes(b[i + 7:i + 9], "big"), \
        int.from_bytes(b[i + 5:i + 7], "big")


@pytest.fixture(scope="module")
def tools():
    make("-C", FTDEV)
    make("-C", FTWEB)
    make("-C", LIBFT, "test/window_dump")
    return FTDEV / "ftdev", FTWEB / "ftweb_cli"


def windows(d):
    """three window scans of the synthetic strip as frames 1, 2, 3; the
    third with another film base"""
    d.mkdir()
    subprocess.run([str(LIBFT / "test" / "window_dump"),
                    str(d / "job_win01")], check=True)
    for n in (2, 3):
        w = d / ("job_win%02d.raw" % n)
        shutil.copy(d / "job_win01.raw", w)
        shutil.copy(d / "job_win01.raw.cal", str(w) + ".cal")
        txt = (d / "job_win01.raw.txt").read_text()
        txt = txt.replace("share_frame=1 ", "share_frame=%d " % n)
        if n == 3:
            txt = txt.replace("film_base=0.2076 0.3979 0.6021",
                              "film_base=0.2150 0.4100 0.6200")
        Path(str(w) + ".txt").write_text(txt)
    (d / "strip.print").write_text(PRINT)
    return [d / ("job_win%02d.raw" % n) for n in (1, 2, 3)]


@pytest.fixture(scope="module")
def both(tools, tmp_path_factory):
    """the CLI's and the engine's outputs for the same windows"""
    ftdev, cli = tools
    tmp = tmp_path_factory.mktemp("ftweb")
    a = windows(tmp / "cli")
    for w in a:
        r = subprocess.run([str(ftdev), "cut", str(w)], capture_output=True,
                           text=True)
        assert r.returncode == 0, r.stderr
    dngs = [str(tmp / "cli" / ("job_frame%02d.dng" % n)) for n in (1, 2, 3)]
    pr = subprocess.run([str(ftdev), "print", *dngs, "--roll",
                         str(tmp / "cli" / "strip.print")],
                        capture_output=True, text=True)
    assert pr.returncode == 0, pr.stderr
    b = windows(tmp / "web")
    out = tmp / "web" / "out"
    out.mkdir()
    wr = subprocess.run([str(cli), "--print", str(tmp / "web" / "strip.print"),
                         "-o", str(out), "--display", "2", "400",
                         *map(str, b)], capture_output=True, text=True)
    assert wr.returncode == 0, wr.stderr
    return tmp / "cli", out, pr, wr


@needs_cc
def test_engine_makes_the_cli_jpegs(both):
    cli, web, _, _ = both
    for n in (1, 2, 3):
        name = "job_frame%02d.jpg" % n
        a, b = (cli / name).read_bytes(), (web / name).read_bytes()
        assert a[:2] == b"\xff\xd8" and len(a) > 10000
        assert a == b, name
        assert (cli / (name + ".txt")).read_bytes() == \
            (web / (name + ".txt")).read_bytes()
    # the clicks and the roll balance reached the prints
    t2 = (web / "job_frame02.jpg.txt").read_text()
    assert "\ncmyd=3 -2 0 1\n" in t2
    # the turn: frame 2 portrait, the others as scanned
    assert t2.endswith("\nrotate=90\n")
    assert "rotate=" not in (web / "job_frame01.jpg.txt").read_text()
    w1, h1 = jpeg_size((web / "job_frame01.jpg").read_bytes())
    w2, h2 = jpeg_size((web / "job_frame02.jpg").read_bytes())
    assert w1 > h1 and (w2, h2) == (h1, w1)
    assert (web / "job_frame01.jpg").read_bytes() != \
        (web / "job_frame02.jpg").read_bytes()


@needs_cc
def test_engine_notes_are_the_cli_warnings(both):
    _, _, pr, wr = both
    assert "[frame 9] is not in the job: ignored" in pr.stderr
    assert "warning: strip.print: [frame 9] is not in the job: ignored" \
        in wr.stderr
    assert "note: 3 frames: print a roll's frames together" in pr.stderr
    assert "note: 3 frames: print a roll's frames together" in wr.stderr


@needs_cc
def test_engine_zip_names_frames(tools, tmp_path):
    """a frame's name (strip.print) names its prints in the zip; the
    others keep <base>_frameNN (ftdev's names); the bytes are the same"""
    _, cli = tools
    b = windows(tmp_path / "web")
    pr = tmp_path / "web" / "strip.print"
    pr.write_text(PRINT.replace("[frame 2]\n", "[frame 2]\nname = dog\n"))
    out = tmp_path / "out"
    out.mkdir()
    r = subprocess.run([str(cli), "--print", str(pr), "-o", str(out),
                        *map(str, b)], capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    with zipfile.ZipFile(out / "job.zip") as z:
        assert z.namelist() == [
            "job_frame01.jpg", "job_frame01.jpg.txt",
            "dog.jpg", "dog.jpg.txt",
            "job_frame03.jpg", "job_frame03.jpg.txt"]
        assert z.read("dog.jpg") == (out / "job_frame02.jpg").read_bytes()


@needs_cc
def test_engine_zip_holds_every_print(both):
    cli, web, _, _ = both
    with zipfile.ZipFile(web / "job.zip") as z:
        assert z.testzip() is None
        assert z.namelist() == [
            "job_frame01.jpg", "job_frame01.jpg.txt",
            "job_frame02.jpg", "job_frame02.jpg.txt",
            "job_frame03.jpg", "job_frame03.jpg.txt"]
        for name in z.namelist():
            assert z.read(name) == (cli / name).read_bytes(), name


@needs_cc
def test_engine_display_copy(both):
    _, web, _, wr = both
    line = [l for l in wr.stdout.splitlines() if l.startswith("display ")]
    w, h = map(int, line[0].split()[1].split("x"))
    assert max(w, h) == 400 and h > w             # frame 2 is turned
    data = (web / "display.rgba").read_bytes()
    assert len(data) == w * h * 4
    assert set(data[3::4]) == {255}               # opaque
    assert len(set(data[0::4])) > 20              # an image, not flat


# libft's ft_lens_dof95 (ft_focus.c, the focus survey 2026-10-15/16's
# fit): K (1+m)/m^2 x bin, m = p x scale; K and the default p pinned here
DOF95_K, LENS_P = 980.0, 3.4799e-4
LENS_2P = ("film = 1468601.0\nw = 268105.0\nfl = 317836.0\n"
           "p = 6.9598e-04\n")


def dof95(scale, b, p=LENS_P):
    m = p * scale
    return DOF95_K * (1 + m) / (m * m) * max(b, 1)


def cli_dof(run, *args):
    a = list(map(str, args))
    r = run(a[:-2] + ["--dof95"] + a[-2:])
    assert r.returncode == 0, r.stdout + r.stderr
    return float(r.stdout)


@needs_cc
def test_engine_dof95_is_libft(tools, tmp_path):
    """ftw_dof95 is libft's law on the page's lens (default: libft's),
    the survey's widths within 10 % at bin 1, bin 2 twice bin 1"""
    cli = tools[1]
    run = lambda a: subprocess.run([str(cli), *a], capture_output=True,
                                   text=True)
    for scale, b in ((6300, 1), (2000, 1), (1500, 1), (1729, 1), (3150, 2),
                     (3150, 0)):
        assert cli_dof(run, scale, b) == pytest.approx(dof95(scale, b),
                                                       abs=1e-5)
    for scale, measured in ((6300, 636), (2000, 3651), (1500, 5653)):
        assert cli_dof(run, scale, 1) == pytest.approx(measured, rel=0.10)
    assert cli_dof(run, 3150, 2) == pytest.approx(2 * cli_dof(run, 3150, 1),
                                                  abs=1e-5)
    lens = tmp_path / "focus.txt"
    lens.write_text(LENS_2P)
    assert cli_dof(run, "--lens", str(lens), 3000, 1) == \
        pytest.approx(dof95(3000, 1, 2 * LENS_P), abs=1e-5)


PPIS = (1220, 1730, 2450, 3000, 3460, 4000)     # the landing page's


def cli_json(run, *args):
    r = run(list(map(str, args)))
    assert r.returncode == 0, r.stdout + r.stderr
    return json.loads(r.stdout)


@needs_cc
def test_engine_resolutions_dmax_is_libft(tools):
    """the landing page's Dmax is libft's estimator (ft_dmax_for_ppi, on
    today's optics limits) to one decimal; null where it has none"""
    run = lambda a: subprocess.run([str(tools[1]), *a], capture_output=True,
                                   text=True)
    res = cli_json(run, "--resolutions", *PPIS)
    assert [r["ppi"] for r in res] == list(PPIS)
    for r in res:
        est = cli_json(run, "--dmax", r["ppi"])
        assert est is not None
        assert r["dmax"] == float("%.1f" % est)
    assert cli_json(run, "--dmax", 6301) is None
    assert cli_json(run, "--dmax", 0) is None


WASI_SDK = Path(os.environ.get(
    "WASI_SDK", Path.home() / ".local/opt/wasi-sdk-34.0-x86_64-linux"))


def node_bin():
    """a node of 22 or later: NODE, the PATH's, /usr/bin/node. Node 20's
    V8 crashes in its GC while the wasm memory grows (SIGSEGV)."""
    for n in (os.environ.get("NODE"), shutil.which("node"), "/usr/bin/node"):
        if not n or not Path(n).exists():
            continue
        r = subprocess.run([n, "-p", "process.versions.node.split('.')[0]"],
                           capture_output=True, text=True)
        if r.returncode == 0 and int(r.stdout) >= 22:
            return n
    return None


NODE = node_bin()
needs_wasm = pytest.mark.skipif(
    not (WASI_SDK / "bin" / "clang").exists() or NODE is None,
    reason="needs the wasi-sdk (WASI_SDK) and node >= 22 (NODE)")

# libc's own (stdio flushed at exit, the clock): the page stubs them.
# Nothing that opens or reads files.
WASM_IMPORTS = {"clock_time_get", "fd_close", "fd_seek", "fd_write"}


@pytest.fixture(scope="module")
def wasm(tools):
    make("-C", FTWEB, "wasm", "WASI_SDK=%s" % WASI_SDK)
    return FTWEB / "ftweb.wasm", FTWEB / "ftweb_cli.wasm"


@needs_cc
@needs_wasm
def test_wasm_module_imports_and_exports(wasm):
    js = ("const m=new WebAssembly.Module(require('fs').readFileSync("
          "process.argv[1]));console.log(JSON.stringify([WebAssembly.Module"
          ".imports(m),WebAssembly.Module.exports(m).map(e=>e.name)]))")
    r = subprocess.run([NODE, "-e", js, str(wasm[0])], capture_output=True,
                       text=True)
    assert r.returncode == 0, r.stderr
    imports, exports = json.loads(r.stdout)
    assert {i["module"] for i in imports} == {"wasi_snapshot_preview1"}
    assert {i["name"] for i in imports} == WASM_IMPORTS
    for name in ("ftw_new", "ftw_cut", "ftw_measure", "ftw_jpeg",
                 "ftw_display", "ftw_zip", "ftw_alloc", "ftw_dof95",
                 "memory"):
        assert name in exports
    assert wasm[0].stat().st_size < 400_000       # light: ~200 KB


@needs_cc
@needs_wasm
def test_wasm_makes_the_native_bytes(wasm, both, tmp_path):
    _, native, _, wr = both
    b = windows(tmp_path / "web")
    out = tmp_path / "web" / "out"
    out.mkdir()
    r = subprocess.run([NODE, "--no-warnings", str(FTWEB / "wasi_run.mjs"),
                        str(wasm[1]), str(tmp_path), "--print",
                        str(tmp_path / "web" / "strip.print"), "-o", str(out),
                        "--display", "2", "400", *map(str, b)],
                       capture_output=True, text=True)
    assert r.returncode == 0, r.stdout + r.stderr
    names = sorted(p.name for p in native.iterdir())
    assert names == sorted(p.name for p in out.iterdir())
    for name in names:
        assert (out / name).read_bytes() == (native / name).read_bytes(), \
            name
    assert r.stderr == wr.stderr


@needs_cc
@needs_wasm
def test_wasm_dof95_is_native(wasm, tools, tmp_path):
    lens = tmp_path / "focus.txt"
    lens.write_text(LENS_2P)
    for args in (["--dof95", "1729", "1"], ["--dof95", "3150", "2"],
                 ["--lens", str(lens), "--dof95", "3000", "1"],
                 ["--resolutions", *map(str, PPIS)], ["--dmax", "1220"]):
        n = subprocess.run([str(tools[1]), *args], capture_output=True,
                           text=True)
        w = subprocess.run([NODE, "--no-warnings",
                            str(FTWEB / "wasi_run.mjs"), str(wasm[1]),
                            str(tmp_path), *args], capture_output=True,
                           text=True)
        assert n.returncode == 0 and w.returncode == 0, w.stderr
        assert w.stdout == n.stdout
