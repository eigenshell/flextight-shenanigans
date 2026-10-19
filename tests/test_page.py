# SPDX-License-Identifier: GPL-2.0-or-later
"""The web page (tools/ftwebd/www): its worker (ftweb.js, the engine's
wasm behind messages) under node, and the whole page end to end in a
headless Chromium against ftwebd and the fake ftpoc: the JPEGs it
downloads are ftdev cut + ftdev print --roll's, byte for byte. Skipped
without the wasi-sdk, node >= 22 (and Chromium for the end-to-end)."""
import json
import re
import shutil
import socket
import subprocess
import threading
import zipfile
from pathlib import Path

import pytest

from tests.test_ftweb import (FTWEB, LENS_2P, LENS_P, NODE, PPIS, WASI_SDK,
                               dof95, make)
from tests.test_ftwebd import Server

ROOT = Path(__file__).resolve().parent.parent
WWW = ROOT / "tools" / "ftwebd" / "www"
FIX = ROOT / "tests" / "fixtures" / "ftwebd"
FTDEV = ROOT / "tools" / "ftdev" / "ftdev"
CHROME = next((c for c in (shutil.which(n) for n in (
    "chromium", "chromium-browser", "google-chrome", "google-chrome-stable"))
    if c), None)
pytestmark = pytest.mark.skipif(
    shutil.which("make") is None or shutil.which("cc") is None or
    not (WASI_SDK / "bin" / "clang").exists() or NODE is None,
    reason="needs make, cc, the wasi-sdk (WASI_SDK) and node >= 22 (NODE)")


@pytest.fixture(scope="module")
def built():
    make("-C", ROOT / "tools" / "ftweb", "www", "WASI_SDK=%s" % WASI_SDK)
    make("-C", ROOT / "tools" / "ftwebd")
    make("-C", ROOT / "tools" / "ftdev")
    make("-C", ROOT / "libft", "test/window_dump")
    return WWW / "ftweb.wasm"


def native_resolutions():
    """the native engine's landing-page resolutions (ftweb_cli)"""
    make("-C", FTWEB)
    r = subprocess.run([str(FTWEB / "ftweb_cli"), "--resolutions",
                        *map(str, PPIS)], capture_output=True, text=True)
    assert r.returncode == 0, r.stdout + r.stderr
    return json.loads(r.stdout)


def worker(tmp, calls):
    """the page's worker runs calls (worker_run.mjs) -> one reply each"""
    p = tmp / "calls.json"
    p.write_text(json.dumps(calls))
    r = subprocess.run([NODE, "--no-warnings", str(FIX / "worker_run.mjs"),
                        str(WWW), str(p)], capture_output=True, text=True)
    assert r.returncode == 0, r.stdout + r.stderr
    return json.loads(r.stdout)


def test_worker_plans_as_ftdev(built, tmp_path):
    """the page's planner (ftw_plan through the worker) writes ftdev
    plan's job; every plan starts again from the preview's job"""
    pv = (FIX / "preview.job").read_text()
    sizes = [2450, 2450, 3000, 0, 2450, 0]
    rep = worker(tmp_path, [
        {"op": "job", "a": [pv]},
        {"op": "plan", "a": [sizes]},
        {"op": "plan", "a": [[0, 0, 0, 3000, 0, 0]]},
        {"op": "plan", "a": [sizes]},
        {"op": "jobJson", "a": ["px=666\nx=2\nbin=12\n"]},
    ])
    assert all("r" in x for x in rep), rep
    first, again = rep[1]["r"], rep[3]["r"]
    assert first == again                     # from the base, not the plan
    assert rep[2]["r"]["windows"] == 1
    shutil.copy(FIX / "preview.job", tmp_path / "strip.job")
    r = subprocess.run([str(FTDEV), "plan", str(tmp_path / "strip.job"),
                        "--size", "4mp:1-2,5", "--size", "6mp:3"],
                       capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    assert first["text"] == (tmp_path / "strip.job").read_text()
    assert first["windows"] == int(r.stdout.split("\n")[-2].split()[0])
    j = rep[4]["r"]
    assert j["film"] == "neg" and j["holder"] == "24x217"
    fr = j["frames"]
    assert [f["n"] for f in fr] == [1, 2, 3, 4, 5, 6]
    assert [f["px"] for f in fr] == [2450, 2450, 3000, 0, 2450, 0]
    # each frame's planned ppi (the review says when a frame raises it)
    assert [f["ppi"] for f in fr] == [1729, 1729, 2117, 0, 1729, 0]
    assert fr[0]["group"] == fr[1]["group"] != fr[2]["group"]
    assert fr[5]["type"] == "partial" and not any(f["done"] for f in fr)
    # boxes in strip.png pixels (525 ppi), along the strip in order
    assert all(a["x"] < b["x"] for a, b in zip(fr, fr[1:]))
    assert fr[0]["w"] == pytest.approx(36 / 25.4 * 525, rel=0.02)


def test_worker_job_from_the_box(built, tmp_path):
    """strip.job as the box keeps it (planLoad): its sizes and done
    frames show; without one, no sizes"""
    pv = (FIX / "preview.job").read_text()
    done = pv.replace("done = no", "done = yes", 1)
    rep = worker(tmp_path, [
        {"op": "job", "a": [pv]},
        {"op": "jobJson", "a": [None]},
        {"op": "planLoad", "a": [done]},
        {"op": "jobJson", "a": [None]},
        {"op": "planLoad", "a": ["[strip]\nbogus = 1\n"]},
    ])
    assert [f["px"] for f in rep[1]["r"]["frames"]] == [0] * 6
    fr = rep[3]["r"]["frames"]
    assert fr[0]["done"] and not fr[1]["done"]
    assert fr[0]["px"] == 2450
    assert "bogus" in rep[4]["err"]


def test_worker_frame_box(built, tmp_path):
    """a crop: the frame's box in the preview's pixels (as jobJson gives
    it) moves the base job's window; the base job's text comes back for
    strip.preview.job; plans start from the new box; a box outside the
    holder and an unknown frame are refused"""
    pv = (FIX / "preview.job").read_text()
    meta = "px=666\nx=2\nbin=12\n"
    rep = worker(tmp_path, [
        {"op": "job", "a": [pv]},
        {"op": "jobJson", "a": [meta]},
    ])
    f1 = rep[1]["r"]["frames"][0]
    box = [f1["x"] + 10, f1["y"] + 5, f1["w"] - 20, f1["h"] - 10]
    rep = worker(tmp_path, [
        {"op": "job", "a": [pv]},
        {"op": "frameBox", "a": [1, box, meta]},
        {"op": "jobJson", "a": [meta]},
        {"op": "plan", "a": [[2452, 0, 0, 0, 0, 0]]},
        {"op": "frameBox", "a": [1, [box[0], -500, box[2], box[3]], meta]},
        {"op": "frameBox", "a": [42, box, meta]},
    ])
    assert "[frame 1]" in rep[1]["r"] and "window = " in rep[1]["r"]
    assert "window = 4.000 1.600" not in rep[1]["r"]
    g = rep[2]["r"]["frames"][0]
    assert [round(g[k], 1) for k in "xywh"] == [round(v, 1) for v in box]
    assert rep[3]["r"]["windows"] == 1
    assert "window outside the holder" in rep[4]["err"]
    assert rep[5] == {"err": "frame 42 is not in the job"}


def test_worker_live_rows(built, tmp_path):
    """the live bar: raw preview lines as they arrive -> RGBA rows; the
    first chunk's brightest sample per channel is the white for the whole
    preview (rows painted earlier never change), negatives inverted,
    gamma 1/2.2; chunks of whole lines only"""
    import numpy as np
    px = 4

    first = np.zeros((2, px, 3), "<u2")
    first[0] = (14000, 12000, 10000)           # open light, per channel
    first[1] = (7000, 6000, 5000)              # half of each white
    later = np.full((1, px, 3), 0, "<u2")
    later[0] = (3500, 3000, 2500)              # a quarter
    for n, a in (("a", first), ("b", later)):
        (tmp_path / n).write_bytes(a.tobytes())
    rep = worker(tmp_path, [
        {"op": "liveRows", "a": [{"file": str(tmp_path / "a")}, px, 1, 1],
         "out": str(tmp_path / "a.rgba")},
        {"op": "liveRows", "a": [{"file": str(tmp_path / "b")}, px, 1, 0],
         "out": str(tmp_path / "b.rgba")},
        {"op": "liveRows", "a": [{"file": str(tmp_path / "b")}, px, 0, 0],
         "out": str(tmp_path / "c.rgba")},
        {"op": "liveRows", "a": [{"file": str(tmp_path / "b")}, 5, 1, 0]},
    ])
    a = np.frombuffer((tmp_path / "a.rgba").read_bytes(), "u1")
    assert rep[0]["r"]["h"] == 2 and rep[0]["r"]["w"] == px
    a = a.reshape(2, px, 4)
    assert a[0, 0].tolist() == [0, 0, 0, 255]
    assert a[1, 0].tolist() == [69, 69, 69, 255]
    b = np.frombuffer((tmp_path / "b.rgba").read_bytes(), "u1")
    assert b.reshape(px, 4)[0].tolist() == [119, 119, 119, 255]
    c = np.frombuffer((tmp_path / "c.rgba").read_bytes(), "u1")
    assert c.reshape(px, 4)[0].tolist() == [136, 136, 136, 255]
    assert rep[3] == {"err": "live: not whole lines of 5 px"}


def test_worker_lens(built, tmp_path):
    """the box's focus.txt reaches the planner, checked by C"""
    rep = worker(tmp_path, [
        {"op": "lens", "a": ["film = 1\nw = 2\n"]},
        {"op": "lens", "a": ["# fit\nfilm = 1468601.0\nw = 268105.0\n"
                             "fl = 317836.0\np = 3.4799e-04\n"]},
    ])
    assert rep[0] == {"err": "focus file: needs film, w, fl and p"}
    assert rep[1] == {"r": None}


def test_worker_dof95(built, tmp_path):
    """the depth of field the page shows: ftw_dof95 (libft's law) on the
    box's lens; jobJson's dof per planned frame at its ppi and bin"""
    pv = (FIX / "preview.job").read_text()
    rep = worker(tmp_path, [
        {"op": "dof95", "a": [6300, 1]},
        {"op": "dof95", "a": [3150, 2]},
        {"op": "job", "a": [pv]},
        {"op": "plan", "a": [[2450, 2450, 3000, 0, 0, 0]]},
        {"op": "jobJson", "a": [None]},
        {"op": "lens", "a": [LENS_2P]},
        {"op": "dof95", "a": [3000, 1]},
    ])
    assert rep[0]["r"] == pytest.approx(dof95(6300, 1), abs=1e-9)
    assert rep[1]["r"] == pytest.approx(dof95(3150, 2), abs=1e-9)
    assert rep[6]["r"] == pytest.approx(dof95(3000, 1, 2 * LENS_P),
                                        abs=1e-9)
    text = rep[3]["r"]["text"]
    secs = text.split("\n[frame ")[1:]
    fr = rep[4]["r"]["frames"]
    for f, s in zip(fr, secs):
        ppi = float(s.split("\nppi = ")[1].split()[0])
        b = int(s.split("\nbin = ")[1].split()[0])
        want = round(dof95(ppi * b, b)) if f["px"] else 0
        assert f["dof"] == want, (f, ppi, b)
    assert [f["dof"] > 0 for f in fr] == [True] * 3 + [False] * 3


def test_worker_print_file(built, tmp_path):
    """strip.print through the worker: clicks read and set, the emulsion
    changed (clicks that fit stay), the file text"""
    rep = worker(tmp_path, [
        {"op": "emulsions"},
        {"op": "printLoad", "a": ["emulsion = generic-neg\n\n[frame 2]\n"
                                  "cmyd = 3 -2 0 1\n"]},
        {"op": "printGet", "a": [2]},
        {"op": "printGet", "a": [5]},
        {"op": "clicks", "a": [5, [0, 1, 0, -1]]},
        {"op": "printText"},
        {"op": "printEmulsion", "a": ["generic-neg"]},
        {"op": "printEmulsion", "a": ["nope"]},
        {"op": "clicks", "a": [5, [100, 0, 0, 0]]},
        {"op": "rotate", "a": [2, 270]},
        {"op": "rotation", "a": [2]},
        {"op": "rotation", "a": [7]},
        {"op": "rotate", "a": [7, 45]},
        {"op": "printEmulsion", "a": ["generic-bw"]},
        {"op": "printText"},
    ])
    ems = rep[0]["r"]
    assert {"id", "name", "mode"} <= set(ems[0])
    assert any(e["id"] == "generic-neg" for e in ems)
    assert rep[2]["r"] == [3, -2, 0, 1]
    assert rep[3]["r"] == [0, 0, 0, 0]
    assert rep[5]["r"] == ("emulsion = generic-neg\n\n[frame 2]\n"
                           "cmyd = 3 -2 0 1\n\n[frame 5]\ncmyd = 0 1 0 -1\n")
    assert rep[6]["r"] == 0                   # nothing dropped
    assert rep[7] == {"err": "not an emulsion"}
    assert rep[8] == {"err": "cmyd: C 100 is out of range (-99..99)"}
    # the turn: read and set; it stays when the emulsion drops the clicks
    assert rep[10]["r"] == 270 and rep[11]["r"] == 0
    assert rep[12] == {"err": "rotate: 45 is not 0, 90, 180 or 270"}
    assert rep[13]["r"] == 2                  # both frames' clicks dropped
    assert rep[14]["r"] == ("emulsion = generic-bw\n\n[frame 2]\n"
                            "rotate = 270\n")


def test_worker_frame_names(built, tmp_path):
    """a frame's name through the worker: set, read, cleared, refused"""
    rep = worker(tmp_path, [
        {"op": "printNew", "a": ["generic-neg"]},
        {"op": "name", "a": [3]},
        {"op": "setName", "a": [3, "2026-10-18_a_03"]},
        {"op": "name", "a": [3]},
        {"op": "printText"},
        {"op": "setName", "a": [3, "no way"]},
        {"op": "setName", "a": [3, ""]},
        {"op": "printText"},
    ])
    assert rep[1]["r"] == ""
    assert rep[3]["r"] == "2026-10-18_a_03"
    assert rep[4]["r"] == ("emulsion = generic-neg\n\n[frame 3]\n"
                           "name = 2026-10-18_a_03\n")
    assert rep[5] == {"err": "name: 'no way' is not 1..64 of A-Z a-z 0-9 "
                             ". _ - (not starting with .)"}
    assert rep[7]["r"] == "emulsion = generic-neg\n"


def test_worker_resolutions_and_holders(built, tmp_path):
    """the landing page's choices come from C: MP and the planner's px per
    ppi (36x24 geometry), Dmax the native engine's (libft's estimator),
    a ppi no bin reaches refused; the holders with their labels"""
    rep = worker(tmp_path, [
        {"op": "resolutions", "a": [list(PPIS)]},
        {"op": "resolutions", "a": [[500]]},
        {"op": "holders"},
    ])
    assert [{k: v for k, v in r.items() if k != "dmax"}
            for r in rep[0]["r"]] == [
        {"ppi": 1220, "mp": 2.0, "px": 1729},
        {"ppi": 1730, "mp": 4.0, "px": 2452},
        {"ppi": 2450, "mp": 8.0, "px": 3472},
        {"ppi": 3000, "mp": 12.1, "px": 4252},
        {"ppi": 3460, "mp": 16.0, "px": 4904},
        {"ppi": 4000, "mp": 21.4, "px": 5669}]
    assert rep[0]["r"] == native_resolutions()
    assert all(r["dmax"] is not None for r in rep[0]["r"])
    assert rep[1] == {"err": "500 ppi: out of the optics' range"}
    assert rep[2]["r"] == [{
        "name": "24x217", "frames": 6,
        "label": "Filmchrome 24x36x6 (25.2 x 226.8 mm), 6 frames 35 mm "
                 "with borders"}]


BASE_D = (0.21, 0.40, 0.60)          # an orange mask: D r g b


def preview_files(d, boxes, px=666, lines=4797):
    """a synthetic standard preview (strip.raw, .cal, .txt) of the
    preview job's frames: film base outside, each frame dense (a
    negative's highlight) along its first half, thin along its second"""
    import numpy as np
    dark, white = 400.0, 14000.0
    dens = np.zeros((lines, px, 3))
    dens[:] = BASE_D
    for b in boxes:
        l0, l1 = int(b["x"]), int(b["x"] + b["w"])
        c0, c1 = int(b["y"]), int(b["y"] + b["h"])
        mid = (l0 + l1) // 2
        dens[l0:mid, c0:c1] += 1.2
        dens[mid:l1, c0:c1] += 0.3
    raw = dark + 10 ** -dens * (white - dark)
    cal = np.concatenate([np.full((200, px, 3), dark),
                          np.full((50, px, 3), white)])
    (d / "strip.raw").write_bytes(raw.round().astype("<u2").tobytes())
    (d / "strip.raw.cal").write_bytes(cal.astype("<u2").tobytes())
    (d / "strip.raw.txt").write_text("px=%d\nx=2\nbin=12\n" % px)
    return d / "strip.raw", d / "strip.raw.cal", d / "strip.raw.txt"


def test_worker_positive_preview(built, tmp_path):
    """the preview's own raw printed (no clicks): every frame of the
    preview's job, its box's size, a positive (the dense half of the
    negative prints light); the film base from the job or estimated;
    an emulsion of the other film refused"""
    pytest.importorskip("numpy")
    pv = (FIX / "preview.job").read_text()
    meta = "px=666\nx=2\nbin=12\n"
    boxes = worker(tmp_path, [{"op": "job", "a": [pv]},
                              {"op": "jobJson", "a": [meta]}])[1]["r"]["frames"]
    raw, cal, txt = preview_files(tmp_path, boxes)
    based = pv.replace("hf_half_res = no\n", "hf_half_res = no\nfilm_base = "
                       "%.4f %.4f %.4f\n" % BASE_D)
    files = [{"file": str(raw)}, {"file": str(cal)}, meta, None]
    rep = worker(tmp_path, [
        {"op": "printNew", "a": ["generic-neg"]},
        {"op": "job", "a": [pv]},
        {"op": "previewLoad", "a": files},
        {"op": "previewDisplay", "a": [1, 2000],
         "out": str(tmp_path / "f1.rgba")},
        {"op": "previewDisplay", "a": [3, 120]},
        {"op": "previewDisplay", "a": [9, 120]},
        {"op": "job", "a": [based]},
        {"op": "previewLoad", "a": files},
        {"op": "previewDisplay", "a": [1, 2000],
         "out": str(tmp_path / "b1.rgba")},
        {"op": "printNew", "a": ["generic-slide"]},
        {"op": "previewDisplay", "a": [1, 120]},
        {"op": "previewLoad", "a": [files[0], files[1], "px=666\nx=2\nbin=6\n",
                                    None]},
    ])
    assert rep[2]["r"] == 6
    d = rep[3]["r"]
    b = boxes[0]
    assert abs(d["w"] - b["w"]) <= 2 and abs(d["h"] - b["h"]) <= 2
    assert d["w"] > d["h"]                    # along the strip, as shown
    assert max(rep[4]["r"]["w"], rep[4]["r"]["h"]) == 120
    assert rep[5] == {"err": "no such frame in the preview"}
    for name in ("f1.rgba", "b1.rgba"):
        px = (tmp_path / name).read_bytes()
        assert len(px) == d["w"] * d["h"] * 4
        row = d["w"] * 4
        y = d["h"] // 2
        line = px[y * row:(y + 1) * row]
        lum = [sum(line[i:i + 3]) for i in range(0, row, 4)]
        q = d["w"] // 4
        dense, thin = sum(lum[q // 2:q]), sum(lum[3 * q:3 * q + q // 2])
        assert dense > thin * 1.5, name      # a positive: dense prints light
    assert rep[7]["r"] == 6
    assert "slide" in rep[10]["err"]
    assert "standard preview" in rep[11]["err"]


def test_worker_preview_print(built, tmp_path):
    """the review shows a frame as it will print before it is scanned:
    the preview frame with strip.print's clicks and turn (previewDisplay
    stays without them); the standard preview's px for the live bar"""
    pytest.importorskip("numpy")
    pv = (FIX / "preview.job").read_text()
    meta = "px=666\nx=2\nbin=12\n"
    boxes = worker(tmp_path, [{"op": "job", "a": [pv]},
                              {"op": "jobJson", "a": [meta]}])[1]["r"]["frames"]
    raw, cal, txt = preview_files(tmp_path, boxes)
    files = [{"file": str(raw)}, {"file": str(cal)}, meta, None]
    rep = worker(tmp_path, [
        {"op": "printNew", "a": ["generic-neg"]},
        {"op": "job", "a": [pv]},
        {"op": "previewLoad", "a": files},
        {"op": "previewPrint", "a": [1, 300], "out": str(tmp_path / "a")},
        {"op": "clicks", "a": [1, [0, 0, 0, 6]]},
        {"op": "previewPrint", "a": [1, 300], "out": str(tmp_path / "b")},
        {"op": "previewDisplay", "a": [1, 300], "out": str(tmp_path / "c")},
        {"op": "rotate", "a": [1, 90]},
        {"op": "previewPrint", "a": [1, 300]},
        {"op": "previewPx"},
    ])
    a, b, c = ((tmp_path / n).read_bytes() for n in "abc")
    assert a == c                             # no clicks yet: the same
    assert b != a and sum(b) < sum(a)         # D + 6: a darker print
    w, h = rep[3]["r"]["w"], rep[3]["r"]["h"]
    assert (rep[8]["r"]["w"], rep[8]["r"]["h"]) == (h, w)
    assert rep[9]["r"] == 666


class Proxy:
    """a TCP pipe in front of ftwebd (one request per connection, as
    ftwebd answers), the box's slow Wi-Fi breaking: rule(head) -> None
    (passed), or n: the answer cut after n bytes (0: the connection
    dropped unanswered). GET /_proxy/open (from the page) sets .open"""
    def __init__(self, port, rule):
        self.up, self.rule, self.open, self.cut = port, rule, False, []
        self.ls = socket.create_server(("127.0.0.1", 0))
        self.port = self.ls.getsockname()[1]
        threading.Thread(target=self.loop, daemon=True).start()

    def loop(self):
        while True:
            try:
                c, _ = self.ls.accept()
            except OSError:
                return
            threading.Thread(target=self.one, args=(c,), daemon=True).start()

    def one(self, c):
        data = b""
        while b"\r\n\r\n" not in data:
            d = c.recv(65536)
            if not d:
                return c.close()
            data += d
        head = data.split(b"\r\n\r\n")[0].decode("latin-1")
        if head.startswith("GET /_proxy/open "):
            self.open = True
            c.sendall(b"HTTP/1.1 204 No Content\r\nConnection: close\r\n\r\n")
            return c.close()
        n = self.rule(self, head)
        if n is not None:
            self.cut.append((head.split("\r\n")[0], n))
        if n == 0:
            return c.close()
        u = socket.create_connection(("127.0.0.1", self.up))
        u.sendall(data)

        def up():                    # the rest of the request, if any
            try:
                while True:
                    d = c.recv(65536)
                    if not d:
                        break
                    u.sendall(d)
            except OSError:
                pass
        threading.Thread(target=up, daemon=True).start()
        sent = 0
        try:
            while True:
                d = u.recv(65536)
                if not d:
                    break
                if n is not None and sent + len(d) >= n:
                    c.sendall(d[:n - sent])
                    break
                c.sendall(d)
                sent += len(d)
        except OSError:
            pass
        for x in (u, c):
            try:
                x.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            x.close()

    def close(self):
        self.ls.close()


def run_page(built, tmp_path, rule=None, mode="full"):
    """the page in Chromium: landing (2450 ppi), SCAN, the live bar, the
    review by keys (qwerty on frame 1, dvorak on frame 2 with a name), a
    crop dragged on frame 3, 4 and 5 skipped, 6 confirmed: the scan
    starts by itself; Download and All; then a crop on a scanned frame:
    the dialog, R, its rescan. rule: a Proxy's between page and box;
    mode "fail": the scan's window does not come, then does"""
    pytest.importorskip("numpy")
    meta = "px=666\nx=2\nbin=12\n"
    boxes = worker(tmp_path, [
        {"op": "job", "a": [(FIX / "preview.job").read_text()]},
        {"op": "jobJson", "a": [meta]}])[1]["r"]["frames"]
    (tmp_path / "pv").mkdir()
    preview_files(tmp_path / "pv", boxes)
    win = tmp_path / "wd" / "win"
    win.parent.mkdir()
    subprocess.run([str(ROOT / "libft" / "test" / "window_dump"), str(win)],
                   check=True)
    box = tmp_path / "box"
    box.mkdir()
    dl = tmp_path / "dl"
    dl.mkdir()
    hold = tmp_path / "hold"
    srv = Server(ROOT / "tools" / "ftwebd" / "ftwebd", box, 0.03,
                 env={"FAKE_FTPOC_WINDOW": str(win),
                      "FAKE_FTPOC_PREVIEW": str(tmp_path / "pv" / "strip"),
                      "FAKE_FTPOC_HOLD": str(hold)})
    px = Proxy(srv.port, rule) if rule else None
    try:
        r = subprocess.run([NODE, "--no-warnings", str(FIX / "page_e2e.mjs"),
                            CHROME, "http://127.0.0.1:%d/" %
                            (px or srv).port, str(dl), mode, str(hold)],
                           capture_output=True, text=True, timeout=300)
    finally:
        if px:
            px.close()
        hold.unlink(missing_ok=True)
        srv.close()
    assert r.returncode == 0, r.stderr[-4000:]
    out = json.loads(r.stdout)
    out["cut"] = px.cut if px else []
    return out, box, dl


@pytest.mark.skipif(CHROME is None, reason="needs Chromium")
def test_page_end_to_end(built, tmp_path):
    check_end_to_end(tmp_path, *run_page(built, tmp_path))


WIN = re.compile(r"GET /api/strips/[^/ ]+/files/strip_j\d+_win01\.raw ")


def broken(px, head):
    """the window's first GET (the whole file) cut after 3 MB, the next
    (the resume) dropped unanswered; the rest through"""
    if not WIN.match(head):
        return None
    px.n = getattr(px, "n", 0) + 1
    return {1: 3 << 20, 2: 0}.get(px.n)


@pytest.mark.skipif(CHROME is None, reason="needs Chromium")
def test_page_resumes_a_broken_transfer(built, tmp_path):
    """the box's Wi-Fi breaks while the page fetches a window (hundreds
    of MB at ~100 KB/s on the box): the page goes on from the bytes it
    has (Range), and its prints are the CLI's as before"""
    out, box, dl = run_page(built, tmp_path, broken)
    assert [n for _, n in out["cut"]] == [3 << 20, 0]
    check_end_to_end(tmp_path, out, box, dl)


@pytest.mark.skipif(CHROME is None, reason="needs Chromium")
def test_page_batch_queue(built, tmp_path):
    """the first confirm starts a job; frames confirmed while it scans
    collect into the next job (the batch's tail, re-planned by PUT while
    it is queued), which runs after it"""
    out, box, dl = run_page(built, tmp_path, mode="batch")
    s = out["strip"]
    # a full batch (507): re-armed once a job ended, not by every snapshot
    assert out["rearm"] == [[0, 0, 1, 1], 0]
    # adopted jobs: a failed fetch tried again; an ended job without its
    # file leaves its confirmed frames to retry
    assert out["adopt"] == [False, True, "retry", []]
    assert out["first"] == [[2, "running", [1]]]
    assert out["jobs"] == [[2, "running", [1]], [3, "queued", [2, 3]]]
    inc = dict(re.findall(r"\[frame (\d)\]\ntype = \w+\ninclude = (\w+)",
                          out["tailJob"]))
    assert inc == {"1": "no", "2": "yes", "3": "yes", "4": "no", "5": "no",
                   "6": "no"}
    assert out["badges"] == ["scanning", "queued", "queued"]
    # unconfirm: a queued frame out of its job (PUT), the job's last
    # frame out drops it (DELETE); a frame of the running job cannot
    # (Skip disabled, Stop offered)
    inc = dict(re.findall(r"\[frame (\d)\]\ntype = \w+\ninclude = (\w+)",
                          out["tailAfter"]))
    assert (inc["2"], inc["3"]) == ("yes", "no")
    assert out["badge3"] == "pending"
    assert out["dropped"] is None
    assert out["skipOff"] == [True, False]
    assert out["running"] == ("frame 1 is in the job scanning now: Stop "
                              "ends that job (its frames to retry)")
    assert out["skipOn"] is False
    # while the batch's changes wait, the last decision holds: a frame
    # unconfirmed before its enqueue ran is not sent; one confirmed again
    # before its unconfirm ran stays in its job
    assert out["late"] == [False, "pending"]
    assert out["again"] == [[2], "confirmed"]
    # a reload: the same jobs, rebuilt from the box
    assert out["before"] == out["after"] == [[2, "running", [1]],
                                             [4, "queued", [2]]]
    assert out["badgesAfter"] == ["scanning", "queued", "pending"]
    assert out["bat"] == "Scanning · 2 frames left"
    # stopped: frame 1 to retry; Confirm sent it again (job 5)
    assert out["retry"] == "retry"
    assert out["landed"] == [1, 2]
    # Clear: job 7 (frame 5) dropped before it ran
    assert out["cleared"] == ["scanning", "retry"]
    assert out["landed2"] == [1, 2, 4]
    files = set(out["files"])
    assert {"strip_j4_win01.raw", "strip_j5_win01.raw"} <= files
    assert "strip_j2_win01.raw" not in files  # stopped before its window
    assert "strip_j3.job" not in files       # the dropped job's file
    log = [json.loads(l) for l in (box / "fake_ftpoc.log").read_text()
           .splitlines()]
    assert [a[2] for a in log if a[0] == "scan"] == \
        ["strips/%s/strip_j%d.job" % (s, i) for i in (2, 4, 5, 6)]


def lost(px, head):
    """the window does not come at all until the page opens the proxy"""
    return 0 if WIN.match(head) and not px.open else None


@pytest.mark.skipif(CHROME is None, reason="needs Chromium")
def test_page_lost_window_does_not_wedge(built, tmp_path):
    """a window that cannot be fetched: said, by name and how far it got;
    no rescan started for it; a window landing after it still lands, its
    frame's print the CLI's"""
    out, box, dl = run_page(built, tmp_path, lost, "fail")
    assert out["cut"] and all(n == 0 for _, n in out["cut"])
    assert re.match(r"strip_j2_win01\.raw: stopped at 0 MB: ", out["bad"]), \
        out["bad"]
    assert out["landedFail"] == [] and out["landed"] == [1]
    assert not out["restarted"]
    log = [json.loads(l) for l in (box / "fake_ftpoc.log").read_text()
           .splitlines()]
    assert [a[0] for a in log] == ["frames", "scan"]
    assert out["badAfter"] == ""
    s = out["strip"]
    strip = box / "strips" / s
    job = tmp_path / "job1.job"
    job.write_text(out["job1"])
    subprocess.run([str(FTDEV), "cut", str(strip / "strip_j2_win01.raw")],
                   check=True, capture_output=True)
    r = subprocess.run([str(FTDEV), "print", str(strip / "strip_j2_frame01.dng"),
                        "--roll", str(strip / "strip.print"), "--job",
                        str(job)], capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    assert (dl / (s + "_frame01.jpg")).read_bytes() == \
        (strip / "strip_j2_frame01.jpg").read_bytes()


def check_end_to_end(tmp_path, out, box, dl):
    s = out["strip"]
    strip = box / "strips" / s
    # the landing page: the engine's numbers, Dmax the estimator's
    # (one decimal; #N/A where it has none)
    want = ["%dppi · %.1f MPDmax%s" % (
        r["ppi"], r["mp"],
        ": #N/A" if r["dmax"] is None else " ≈ %.1f" % r["dmax"])
        for r in native_resolutions()]
    assert out["res"] == want
    assert out["res"][2].startswith("2450ppi · 8.0 MPDmax ≈ ")
    assert out["holder"].startswith("Filmchrome 24x36x6")
    assert out["recent"] == "coming soon"
    assert re.match(r"type = quick\nppi = 2450\nholder = 24x217\n"
                    r"emulsion = generic-neg\ncreated = ", out["scanFile"])
    # the review: keys (qwerty, then dvorak), a name, decisions kept
    assert out["print"] == ("emulsion = generic-neg\n\n[frame 1]\n"
                            "cmyd = 2 -1 0 0\nrotate = 90\n\n[frame 2]\n"
                            "name = dog\ncmyd = 0 0 0 1\nrotate = 90\n")
    assert out["review"] == ("1 confirmed\n2 confirmed\n3 confirmed\n"
                             "4 skipped\n5 skipped\n6 skipped\n")
    # partial frames wait for per-frame resolution: under construction
    assert out["uc"] == "under construction"
    assert out["ucSaid"] == ("frame 6 (partial): under construction: "
                             "partial frames wait for per-frame resolution")
    # the crop: frame 3's window shorter, the rest as detected
    w0 = re.findall(r"window = (\S+) (\S+) (\S+)", out["before"])
    w1 = re.findall(r"window = (\S+) (\S+) (\S+)", out["after"])
    assert w1[:2] == w0[:2] and w1[3:] == w0[3:]
    assert float(w1[2][2]) < float(w0[2][2]) - 1
    # the scan started by itself: confirmed frames in, skipped and
    # partial out; the cropped frame 3 raises the ppi, and the page says so
    j1 = out["job1"]
    inc = re.findall(r"\[frame (\d)\]\ntype = \w+\ninclude = (\w+)", j1)
    assert inc == [("1", "yes"), ("2", "yes"), ("3", "yes"), ("4", "no"),
                   ("5", "no"), ("6", "no")]
    assert "share = 3472" in j1
    assert out["ppin"] == ("note: this scan runs at 2617 ppi, not 2450: "
                           "every frame gets 3472 px on its long side, and "
                           "frame 3 (cropped) needs more ppi for that")
    assert out["landed1"] == [1]              # one window, one fake frame
    # frames 2 and 3 were in the job, but the fake window holds frame 1
    # only: to retry
    assert out["states"] == ["done", "retry", "retry", "skipped",
                             "skipped", "under construction"]
    seen = "\n".join(out["seen"])
    assert "window 1/1 · focusing frame 1 · crop 1 at -500" in seen
    # the depth of field: the window's peak shaded +-DOF95/2 (its frames'
    # dof, ftw_dof95 at the planned ppi and bin) and said
    sec = j1.split("\n[frame 1]")[1]
    ppi = float(sec.split("\nppi = ")[1].split()[0])
    b = int(sec.split("\nbin = ")[1].split()[0])
    h = round(round(dof95(ppi * b, b)) / 2)
    assert "window 1/1 · focus +250 (measured) · DOF ±%d steps |band %d" \
        % (h, h) in seen, seen
    assert out["plot"]
    # a crop on a scanned frame asked; R rescanned it in a run of its own,
    # in a job of its own (its own window files)
    assert out["question"] == ("Frame 1 is already scanned. A new crop "
                               "needs a rescan.")
    log = [json.loads(l) for l in (box / "fake_ftpoc.log").read_text()
           .splitlines()]
    assert [a[0] for a in log] == ["frames", "scan", "scan"]
    assert {"strip_j2_win01.raw", "strip_j3_win01.raw"} <= set(out["files"])
    assert 1 in out["landed"]
    # the keymap overlay lists the dvorak keys
    assert "'" in out["keymapText"] and "turn right" in out["keymapText"]
    # the downloads (after the first run) are the CLI's prints of it
    job = tmp_path / "job1.job"
    job.write_text(j1)
    for w in sorted(strip.glob("strip_j2_win*.raw")):
        subprocess.run([str(FTDEV), "cut", str(w)], check=True,
                       capture_output=True)
    dngs = [str(strip / "strip_j2_frame01.dng")]
    r = subprocess.run([str(FTDEV), "print", *dngs, "--roll",
                        str(strip / "strip.print"), "--job", str(job)],
                       capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    page = dl / (s + "_frame01.jpg")
    assert page.read_bytes() == (strip / "strip_j2_frame01.jpg").read_bytes()
    assert (dl / (s + "_frame01.jpg.txt")).read_bytes() == \
        (strip / "strip_j2_frame01.jpg.txt").read_bytes()
    assert "\ncmyd=2 -1 0 0\n" in page.with_suffix(".jpg.txt").read_text()
    assert "\nrotate=90\n" in page.with_suffix(".jpg.txt").read_text()
    with zipfile.ZipFile(dl / (s + ".zip")) as z:
        assert z.testzip() is None
        assert z.namelist() == [s + "_frame01.jpg", s + "_frame01.jpg.txt"]
        for n in z.namelist():
            assert z.read(n) == \
                (strip / ("strip_j2" + n[len(s):])).read_bytes(), n
