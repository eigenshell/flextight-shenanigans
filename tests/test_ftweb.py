# SPDX-License-Identifier: GPL-2.0-or-later
"""The web page's engine (tools/ftweb) makes the CLI's JPEGs: ftweb_cli
(the engine driven from files) against ftdev cut + ftdev print --roll on
synthetic window scans, one frame a call (the engine balances each frame
on its own: per-frame balance); and the engine compiled to wasm32 makes
the same bytes as native (skipped without the wasi-sdk or node)."""
import types
import json
import os
import re
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
    # per-frame balance: the CLI's reference prints each frame alone
    err = ""
    for n in (1, 2, 3):
        dng = str(tmp / "cli" / ("job_frame%02d.dng" % n))
        r = subprocess.run([str(ftdev), "print", dng, "--roll",
                            str(tmp / "cli" / "strip.print")],
                           capture_output=True, text=True)
        assert r.returncode == 0, r.stderr
        err += r.stderr
    pr = types.SimpleNamespace(stderr=err)
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
    # the clicks reached the prints
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
    # per-frame balance: one frame is the whole "roll", said by the CLI;
    # the engine balances every frame alone by design and says nothing
    assert "note: 1 frame: print a roll's frames together" in pr.stderr
    assert "print a roll's frames together" not in wr.stderr


@needs_cc
def test_engine_balances_each_frame_alone(tools, tmp_path):
    """a frame's print does not depend on the other frames of the strip:
    frame 1 printed with frame 3 (another film base) beside it is frame 1
    printed alone"""
    _, cli = tools
    b = windows(tmp_path / "web")
    outs = []
    for k, wins in enumerate((b[:1], [b[0], b[2]])):
        out = tmp_path / ("out%d" % k)
        out.mkdir()
        r = subprocess.run([str(cli), "--print", str(tmp_path / "web" /
                                                     "strip.print"),
                            "-o", str(out), *map(str, wins)],
                           capture_output=True, text=True)
        assert r.returncode == 0, r.stderr
        outs.append(out)
    assert (outs[0] / "job_frame01.jpg").read_bytes() == \
        (outs[1] / "job_frame01.jpg").read_bytes()


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


# the streamed preview: a synthetic standard preview (dark 400, white
# 14000; film base, frames with content varying along and across)
PV_FRAMES = [(4, 36), (42, 36), (80, 36), (118, 36), (156, 36), (194, 18)]


def preview_raw(d, frames=PV_FRAMES, px=666, lines=4797):
    """<d>/strip.raw, .raw.cal, .raw.txt: frames at (y0, len) mm along,
    columns 60..560 across (2.9..27.1 mm)"""
    np = pytest.importorskip("numpy")
    ym = 25.4 / 525
    dens = np.zeros((lines, px, 3))
    dens[:] = (0.21, 0.40, 0.60)
    for y0, ln in frames:
        l0, l1 = int(round(y0 / ym)), int(round((y0 + ln) / ym))
        ll = np.arange(l0, l1)[:, None]
        cc = np.arange(60, 560)[None, :]
        pat = 0.5 + 0.4 * np.sin(ll * 0.05 + y0) * np.cos(cc * 0.03)
        dens[l0:l1, 60:560] += (0.3 + 0.9 * pat)[..., None] * \
            np.array([1.0, 0.9, 0.8])
    raw = 400.0 + 10 ** -dens * (14000.0 - 400.0)
    cal = np.concatenate([np.full((200, px, 3), 400.0),
                          np.full((50, px, 3), 14000.0)])
    d.mkdir(parents=True, exist_ok=True)
    (d / "strip.raw").write_bytes(raw.round().astype("<u2").tobytes())
    (d / "strip.raw.cal").write_bytes(cal.astype("<u2").tobytes())
    (d / "strip.raw.txt").write_text("px=%d\nx=2\nbin=12\n" % px)
    return d / "strip"


def stream_run(cli, base, *args):
    """ftweb_cli --stream: [(lines in, json)] per sealing call, the end's
    json, the base job's text"""
    r = subprocess.run([str(cli), "--stream", str(base), *map(str, args)],
                       capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    feeds, end, job = [], None, []
    for line in r.stdout.splitlines(True):
        if line.startswith("feed "):
            k, j = line[5:].split(": ", 1)
            feeds.append((int(k), json.loads(j)))
        elif line.startswith("end: "):
            end = json.loads(line[5:])
        else:
            job.append(line)
    return feeds, end, "".join(job)


@needs_cc
def test_engine_stream_whatever_the_chunking(tools, tmp_path):
    """the preview fed one line a call, 13, 1..400 at random or all at
    once: the same frames, the same base job; each frame sealed while the
    preview is still coming, right after its gap"""
    cli = tools[1]
    base = preview_raw(tmp_path / "pv")
    runs = [stream_run(cli, base, "--chunk", n) for n in (1, 13, 0, 99999)]
    frames = [[f for _, j in fe for f in j["frames"]] + e["frames"]
              for fe, e, _ in runs]
    assert all(f == frames[0] for f in frames[1:])
    assert all(r[2] == runs[0][2] for r in runs[1:])
    fr = frames[0]
    assert [f["type"] for f in fr] == ["full"] * 5 + ["half"]
    assert [f["n"] for f in fr] == [1, 2, 3, 4, 5, 6]
    ym = 25.4 / 525
    one = runs[0][0]                      # one line a call
    assert len(one) == 6 and runs[0][1]["frames"] == []
    for (k, j), (y0, ln) in zip(one, PV_FRAMES):
        f = j["frames"][0]
        # the box: the frame padded 0.3 mm (6.2 lines) along (+-2 lines:
        # the raw is written unshifted, the alignment moves R and G)
        assert abs(f["x"] - (y0 - 0.3) / ym) < 4 and \
            abs(f["w"] - (ln + 0.6) / ym) < 5
        # sealed by the gap's 0.4 mm of lines (9; + 2 of RGB alignment)
        assert (y0 + ln) / ym < k < (y0 + ln) / ym + 14
    assert "film_base = 0.2100 0.4000 0.6000" in runs[0][2]


@needs_cc
def test_engine_stream_is_develop_then_detect(tools, tmp_path):
    """the engine's line-by-line calibration is develop's (flat-field,
    RGB alignment) to the bit: its frames are the detector's on the whole
    developed preview (libft/test/stream_dump), to the 0.001 mm printed"""
    cli = tools[1]
    base = preview_raw(tmp_path / "pv")
    make("-C", LIBFT, "test/stream_dump")
    r = subprocess.run([str(LIBFT / "test" / "stream_dump"), str(base)],
                       capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    want = [list(map(float, l.split()[3:7])) for l in r.stdout.splitlines()
            if l.startswith("frame ")]
    _, _, job = stream_run(cli, base, "--chunk", 0)
    got = [list(map(float, w)) for w in
           re.findall(r"window = (\S+) (\S+) (\S+) (\S+)", job)]
    assert len(got) == len(want) == 6
    for g, w in zip(got, want):
        assert g[0] == w[0] and g[1] == w[2]
        assert abs(g[0] + g[2] - w[1]) < 0.0015
        assert abs(g[1] + g[3] - w[3]) < 0.0015


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
