# SPDX-License-Identifier: GPL-2.0-or-later
"""ftwebd, the scanner box's web server, against a fake ftpoc
(tests/fixtures/ftwebd/fake_ftpoc.py): the argv built per op, the event
stream and its resume, one task at a time, stop, the scan's chain (share
windows, then ARC as a detailed scan), the page's files (ETag) and the
checks on names and bodies (strip.job, strip.print)."""
import json
import os
import re
import shutil
import socket
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "tools" / "ftwebd"
FAKE = ROOT / "tests" / "fixtures" / "ftwebd" / "fake_ftpoc.py"
pytestmark = pytest.mark.skipif(
    shutil.which("make") is None or shutil.which("cc") is None or
    sys.platform == "win32", reason="needs make, a C compiler and POSIX")


@pytest.fixture(scope="module")
def binary():
    r = subprocess.run(["make", "-C", str(SRC)], capture_output=True,
                       text=True)
    assert r.returncode == 0, r.stdout + r.stderr
    return SRC / "ftwebd"


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class Server:
    def __init__(self, binary, cwd, delay, args=(), env=None):
        self.port = free_port()
        env = dict(os.environ, FAKE_FTPOC_DELAY=str(delay), **(env or {}))
        self.cwd = cwd
        self.p = subprocess.Popen(
            [str(binary), "--port", str(self.port), "--ftpoc", str(FAKE),
             "--www", str(SRC / "www"), *map(str, args)], cwd=cwd, env=env,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        for _ in range(100):
            try:
                socket.create_connection(("127.0.0.1", self.port), 0.1).close()
                return
            except OSError:
                time.sleep(0.02)
        raise RuntimeError("ftwebd did not start")

    def req(self, method, path, body=None, raw=False):
        data = None
        if isinstance(body, dict):
            data = urllib.parse.urlencode(body).encode()
        elif isinstance(body, str):
            data = body.encode()
        r = urllib.request.Request("http://127.0.0.1:%d%s" % (self.port, path),
                                   data=data, method=method)
        try:
            with urllib.request.urlopen(r, timeout=10) as f:
                code, text = f.status, f.read()
        except urllib.error.HTTPError as e:
            code, text = e.code, e.read()
        return code, text if raw else json.loads(text)

    def events(self, tid, since=0):
        """[(id, data)] until the end event"""
        out = []
        with urllib.request.urlopen(
                "http://127.0.0.1:%d/api/tasks/%d/events?since=%d" %
                (self.port, tid, since), timeout=20) as f:
            assert f.headers["Content-Type"] == "text/event-stream"
            ev, eid = None, None
            for line in f:
                line = line.decode().rstrip("\n")
                if line.startswith("id: "):
                    eid = int(line[4:])
                elif line.startswith("event: "):
                    ev = line[7:]
                elif line.startswith("data: "):
                    if ev == "end":
                        return out
                    out.append((eid, line[6:]))
                elif line == "":
                    ev = None
        return out

    def wait(self, tid):
        for _ in range(500):
            code, t = self.req("GET", "/api/tasks/%d" % tid)
            if t["state"] == "done":
                return t
            time.sleep(0.02)
        raise RuntimeError("task did not end")

    def close(self):
        self.p.kill()
        self.p.wait()


@pytest.fixture
def srv(binary, tmp_path):
    s = Server(binary, tmp_path, 0.001)
    yield s
    s.close()


@pytest.fixture
def slow(binary, tmp_path):
    s = Server(binary, tmp_path, 0.05)
    yield s
    s.close()


def previewed(s, name="a"):
    assert s.req("POST", "/api/strips", {"name": name}) == (200, {"name": name})
    code, t = s.req("POST", "/api/tasks",
                    {"op": "preview", "strip": name, "film": "neg"})
    assert code == 202
    assert s.wait(t["id"])["exit"] == 0
    return t["id"]


def argv_log(s):
    return [json.loads(l) for l in
            (s.cwd / "fake_ftpoc.log").read_text().splitlines()]


def test_preview_argv_events_and_files(srv):
    tid = previewed(srv)
    # the client names an op; ftwebd builds the argv
    assert argv_log(srv) == [["frames", "--film", "neg", "--share", "2450",
                              "--progress", "-o", "strips/a/strip"]]
    ev = srv.events(tid)
    assert [i for i, _ in ev] == list(range(1, len(ev) + 1))
    assert ev[0][1] == "%s frames --film neg --share 2450 --progress -o " \
        "strips/a/strip" % FAKE
    assert ev[1][1] == "progress begin preview 1"
    assert ev[-2][1] == "progress end ok"
    assert ev[-1][1] == "ftwebd exit 0"
    # resume after line k: exactly the rest
    assert srv.events(tid, since=5) == ev[5:]
    d = srv.cwd / "strips" / "a"
    # a good preview keeps its job as the planner's starting point
    assert (d / "strip.preview.job").read_text() == \
        (d / "strip.job").read_text()
    code, files = srv.req("GET", "/api/strips/a/files")
    assert {f["name"] for f in files} >= {"strip.job", "strip.png",
                                          "strip.preview.job"}
    code, png = srv.req("GET", "/api/strips/a/files/strip.png", raw=True)
    assert code == 200 and png.startswith(b"\x89PNG")
    assert srv.req("GET", "/api/strips")[1] == \
        [{"name": "a", "job": True, "png": True, "print": False}]
    code, t = srv.req("GET", "/api/tasks/current")
    assert t == {"id": tid, "op": "preview", "strip": "a", "state": "done",
                 "exit": 0, "stopped": False, "lines": len(ev)}


def test_scan_share_only(srv):
    previewed(srv)
    code, t = srv.req("POST", "/api/tasks", {"op": "scan", "strip": "a"})
    assert code == 202
    assert srv.wait(t["id"])["exit"] == 0
    assert argv_log(srv)[-1] == ["scan", "--job", "strips/a/strip.job",
                                 "--progress"]
    d = srv.cwd / "strips" / "a"
    assert (d / "strip_win01.raw").exists()
    # ftpoc marks the finished windows done in the job
    assert "done = yes" in (d / "strip.job").read_text()


@pytest.fixture
def arc(binary, tmp_path):
    s = Server(binary, tmp_path, 0.001,
               args=["--archive", tmp_path / "nas", "--archive-ppi", "3150"])
    yield s
    s.close()


def test_scan_chain_share_then_archive(arc):
    previewed(arc)
    code, t = arc.req("POST", "/api/tasks", {"op": "scan", "strip": "a",
                                             "arc": "4,6", "film": "neg"})
    assert code == 202
    assert arc.wait(t["id"])["exit"] == 0
    base = str(arc.cwd / "nas" / "a" / "strip")
    assert argv_log(arc)[1:] == [
        ["scan", "--job", "strips/a/strip.job", "--progress"],
        ["frames", "--detail", "neg", "--ppi", "3150", "--film", "neg",
         "--select", "4,6", "--progress", "-o", base],
        ["scan", "--job", base + ".job", "--progress"]]
    # each step's command is an event line, one exit at the end
    ev = [d for _, d in arc.events(t["id"])]
    cmds = [d for d in ev if d.startswith(str(FAKE))]
    assert [c.split()[1] for c in cmds] == ["scan", "frames", "scan"]
    assert ev[0] == cmds[0]
    assert [d for d in ev if d.startswith("ftwebd exit")] == \
        ["ftwebd exit 0"]
    assert (arc.cwd / "nas" / "a" / "strip_win01.raw").exists()
    # archive only: no share step
    code, t = arc.req("POST", "/api/tasks", {"op": "scan", "strip": "a",
                                             "share": "0", "arc": "2",
                                             "film": "slide"})
    assert code == 202 and arc.wait(t["id"])["exit"] == 0
    assert argv_log(arc)[-2][0:2] == ["frames", "--detail"]
    assert argv_log(arc)[-2][5:7] == ["--film", "slide"]


@pytest.mark.parametrize("body, msg", [
    ({"arc": "4"}, "film must be neg or slide"),
    ({"arc": "4; rm", "film": "neg"}, "arc: frame numbers, e.g. 1,3-4"),
    ({"share": "0"}, "nothing to scan"),
])
def test_scan_refusals(arc, body, msg):
    arc.req("POST", "/api/strips", {"name": "a"})
    assert arc.req("POST", "/api/tasks",
                   dict(op="scan", strip="a", **body)) == (422, {"error": msg})
    assert not (arc.cwd / "fake_ftpoc.log").exists()


def test_arc_needs_an_archive_folder(srv):
    srv.req("POST", "/api/strips", {"name": "a"})
    assert srv.req("POST", "/api/tasks", {"op": "scan", "strip": "a",
                                          "arc": "1", "film": "neg"}) == \
        (422, {"error": "no archive folder (ftwebd --archive)"})


def test_stop_ends_the_chain(binary, tmp_path):
    s = Server(binary, tmp_path, 0.05, args=["--archive", tmp_path / "nas"])
    try:
        previewed(s)
        code, t = s.req("POST", "/api/tasks", {"op": "scan", "strip": "a",
                                               "arc": "4", "film": "neg"})
        time.sleep(0.3)
        assert s.req("POST", "/api/tasks/%d/stop" % t["id"])[0] == 200
        done = s.wait(t["id"])
        assert done["stopped"] and done["exit"] == 1
        assert [a[0] for a in argv_log(s)] == ["frames", "scan"]
    finally:
        s.close()


def test_busy_and_stop(slow):
    slow.req("POST", "/api/strips", {"name": "a"})
    code, t = slow.req("POST", "/api/tasks",
                       {"op": "preview", "strip": "a", "film": "slide"})
    assert code == 202
    code, e = slow.req("POST", "/api/tasks", {"op": "scan", "strip": "a"})
    assert code == 409
    assert e["error"] == "scanner busy: preview a (task %d)" % t["id"]
    time.sleep(0.3)
    assert slow.req("POST", "/api/tasks/%d/stop" % t["id"]) == \
        (200, {"ok": True})
    done = slow.wait(t["id"])
    assert done["stopped"] and done["exit"] == 1
    ev = slow.events(t["id"])
    assert [d for _, d in ev[-2:]] == ["progress end stopped",
                                       "ftwebd exit 1"]
    # stopped: not a good preview, nothing kept for the planner
    assert not (slow.cwd / "strips" / "a" / "strip.preview.job").exists()
    assert slow.req("POST", "/api/tasks/%d/stop" % t["id"])[0] == 409
    assert slow.req("POST", "/api/tasks/99/stop")[0] == 404


@pytest.mark.parametrize("body", [
    {"op": "rm", "strip": "a"},
    {"op": "preview", "strip": "a"},                  # no film
    {"op": "preview", "strip": "a", "film": "x; rm"},
    {"op": "scan", "strip": "../a"},
    {"op": "scan"},
])
def test_task_refusals(srv, body):
    srv.req("POST", "/api/strips", {"name": "a"})
    assert srv.req("POST", "/api/tasks", body)[0] == 422
    assert not (srv.cwd / "fake_ftpoc.log").exists()


@pytest.mark.parametrize("name", ["", ".a", "a/b", "..", "a b", "x" * 65])
def test_strip_names(srv, name):
    assert srv.req("POST", "/api/strips", {"name": name})[0] == 422


@pytest.mark.parametrize("path", [
    "/api/strips/a/files/..%2fstrip.job",
    "/api/strips/a/files/.hidden",
    "/api/strips/../files",
    "/..%2f..%2fetc%2fpasswd",
])
def test_traversal_refused(srv, path):
    srv.req("POST", "/api/strips", {"name": "a"})
    assert srv.req("GET", path, raw=True)[0] == 404


def test_job_put_validated(srv):
    previewed(srv)
    d = srv.cwd / "strips" / "a"
    before = (d / "strip.job").read_text()
    code, e = srv.req("PUT", "/api/strips/a/strip.job", "[strip]\nbogus = 1\n")
    assert code == 422 and "bogus" in e["error"]
    assert (d / "strip.job").read_text() == before
    assert srv.req("PUT", "/api/strips/a/strip.job", before) == \
        (200, {"ok": True})


def test_print_put_validated(srv):
    previewed(srv)
    d = srv.cwd / "strips" / "a"
    assert srv.req("GET", "/api/strips/a/strip.print", raw=True)[0] == 404
    code, e = srv.req("PUT", "/api/strips/a/strip.print",
                      "emulsion = generic-neg\n[frame 2]\ncmyd = 1 x\n")
    assert code == 422 and e["error"].startswith("line 3: cmyd:")
    code, e = srv.req("PUT", "/api/strips/a/strip.print", "emulsion = nope\n")
    assert code == 422
    assert not (d / "strip.print").exists()
    good = "emulsion = generic-neg\n\n[frame 2]\ncmyd = 1 0 0 -1\n"
    assert srv.req("PUT", "/api/strips/a/strip.print", good) == \
        (200, {"ok": True})
    assert (d / "strip.print").read_text() == good
    assert srv.req("GET", "/api/strips/a/strip.print", raw=True) == \
        (200, good.encode())
    assert srv.req("GET", "/api/strips")[1][0]["print"] is True


def head_of(s, path, inm=None):
    r = urllib.request.Request("http://127.0.0.1:%d%s" % (s.port, path))
    if inm:
        r.add_header("If-None-Match", inm)
    try:
        with urllib.request.urlopen(r, timeout=10) as f:
            return f.status, dict(f.headers), f.read()
    except urllib.error.HTTPError as e:
        return e.code, dict(e.headers), e.read()


@pytest.mark.parametrize("path", ["/", "/ftweb.js", "/review.js"])
def test_page_files_revalidate(srv, path):
    """the page's files: ETag + no-cache; 304 while unchanged, so the
    browser keeps ftweb.wasm and asks only whether it changed"""
    code, h, body = head_of(srv, path)
    assert code == 200 and h["Cache-Control"] == "no-cache"
    assert re.fullmatch(r'"[0-9a-f]+-[0-9a-f]+"', h["ETag"])
    code, h2, body2 = head_of(srv, path, h["ETag"])
    assert code == 304 and body2 == b"" and h2["ETag"] == h["ETag"]
    assert head_of(srv, path, '"0-0"')[0] == 200


def test_wasm_type(srv, tmp_path):
    www = tmp_path / "www"
    www.mkdir()
    (www / "ftweb.wasm").write_bytes(b"\0asm\1\0\0\0")
    s = Server(srv_binary(), tmp_path, 0.001, args=["--www", www])
    try:
        code, h, body = head_of(s, "/ftweb.wasm")
        assert code == 200 and h["Content-Type"] == "application/wasm"
        assert body == b"\0asm\1\0\0\0"
        assert head_of(s, "/nope.wasm")[0] == 404
    finally:
        s.close()


def srv_binary():
    return SRC / "ftwebd"


def test_focus_txt(srv):
    assert srv.req("GET", "/api/focus.txt", raw=True)[0] == 404
    (srv.cwd / "focus.txt").write_text("film = 1\n")
    assert srv.req("GET", "/api/focus.txt", raw=True) == (200, b"film = 1\n")


def test_build_bat_links(tmp_path):
    """build.bat lists libft's sources by hand (only the box builds it):
    its list, with proc_posix.c for proc_win.c, must link here"""
    line = [l for l in (SRC / "build.bat").read_text().splitlines()
            if l.startswith("gcc ")]
    assert len(line) == 1
    srcs = [str(SRC / f) for f in ("ftwebd.c", "http.c", "task.c",
                                   "proc_posix.c")]
    srcs += [str(ROOT / "libft" / f) for f in
             re.findall(r"%LIBFT%\\(\w+\.c)", line[0])]
    r = subprocess.run(["cc", "-std=c99", "-O0", "-I", str(ROOT / "libft"),
                        "-o", str(tmp_path / "ftwebd"), *srcs, "-lm",
                        "-pthread"], capture_output=True, text=True)
    assert r.returncode == 0, r.stderr


def test_page_fits_the_first_packet(srv):
    code, page = srv.req("GET", "/", raw=True)
    assert code == 200 and page.startswith(b"<!doctype html>")
    assert len(page) < 14 * 1024
    # the rest loads after the shell: the review screen, the worker
    assert b'<script src="review.js" defer>' in page
    assert b'new Worker("ftweb.js")' in page


def ranged(s, path, start):
    """GET with Range: bytes=start- -> (code, headers, body)"""
    r = urllib.request.Request("http://127.0.0.1:%d%s" % (s.port, path))
    r.add_header("Range", "bytes=%d-" % start)
    try:
        with urllib.request.urlopen(r, timeout=10) as f:
            return f.status, dict(f.headers), f.read()
    except urllib.error.HTTPError as e:
        return e.code, dict(e.headers), e.read()


def test_range_reads_a_growing_file(srv):
    """the live bar reads strip.raw as ftpoc writes it: bytes=N- -> 206
    with what is there now (at most 1 MB a reply), 416 when nothing new;
    no Range: the whole file as before"""
    srv.req("POST", "/api/strips", {"name": "a"})
    raw = srv.cwd / "strips" / "a" / "strip.raw"
    raw.write_bytes(bytes(range(256)) * 4)
    p = "/api/strips/a/files/strip.raw"
    code, h, body = ranged(srv, p, 1000)
    assert code == 206 and body == (bytes(range(256)) * 4)[1000:]
    assert h["Content-Range"] == "bytes 1000-1023/1024"
    assert ranged(srv, p, 1024)[0] == 416
    with raw.open("ab") as f:
        f.write(b"\x01" * (3 << 20))
    code, h, body = ranged(srv, p, 1024)
    assert code == 206 and len(body) == 1 << 20
    assert h["Content-Range"] == "bytes 1024-%d/%d" % ((1 << 20) + 1023,
                                                       1024 + (3 << 20))
    assert len(srv.req("GET", p, raw=True)[1]) == 1024 + (3 << 20)


def test_recent_scans_placeholder(srv):
    assert srv.req("GET", "/api/scans") == (200, [])


def test_strip_with_choices(srv):
    """the landing page's choices are kept with the strip (strip.scan);
    bad ones refused, the folder not made"""
    body = {"name": "b", "type": "quick", "ppi": "1730", "holder": "24x217",
            "emulsion": "generic-neg"}
    assert srv.req("POST", "/api/strips", body) == (200, {"name": "b"})
    t = (srv.cwd / "strips" / "b" / "strip.scan").read_text()
    assert re.fullmatch(r"type = quick\nppi = 1730\nholder = 24x217\n"
                        r"emulsion = generic-neg\n"
                        r"created = \d{4}-\d\d-\d\dT\d\d:\d\d:\d\d\n", t)
    for k, v, msg in (("ppi", "500", "ppi: 500 is out of the optics' range"),
                      ("holder", "nope", "holder: no holder 'nope'"),
                      ("emulsion", "nope", "emulsion: no emulsion 'nope'"),
                      ("type", "archive", "type: only quick scans so far")):
        code, e = srv.req("POST", "/api/strips", dict(body, name="c", **{k: v}))
        assert (code, e["error"]) == (422, msg)
    assert not (srv.cwd / "strips" / "c").exists()


def test_review_put_validated(srv):
    srv.req("POST", "/api/strips", {"name": "a"})
    p = "/api/strips/a/strip.review"
    assert srv.req("GET", p, raw=True)[0] == 404
    good = "1 confirmed\n2 skipped\n3 pending\n"
    assert srv.req("PUT", p, good) == (200, {"ok": True})
    assert srv.req("GET", p, raw=True) == (200, good.encode())
    for bad, msg in (("1 maybe\n", "line 1: '1 maybe' is not <frame> "
                                   "pending|confirmed|skipped"),
                     ("x confirmed\n", "line 1: 'x confirmed' is not <frame> "
                                       "pending|confirmed|skipped")):
        code, e = srv.req("PUT", p, bad)
        assert (code, e["error"]) == (422, msg)
    assert srv.req("GET", p, raw=True) == (200, good.encode())


def test_preview_job_put_validated(srv):
    """a crop keeps the preview's job (the base of every plan)"""
    previewed(srv)
    d = srv.cwd / "strips" / "a"
    before = (d / "strip.preview.job").read_text()
    p = "/api/strips/a/strip.preview.job"
    code, e = srv.req("PUT", p, "[strip]\nbogus = 1\n")
    assert code == 422 and "bogus" in e["error"]
    moved = before.replace("window = 4.000 1.600", "window = 5.000 1.600", 1)
    assert moved != before
    assert srv.req("PUT", p, moved) == (200, {"ok": True})
    assert (d / "strip.preview.job").read_text() == moved


def test_scan_keeps_earlier_runs(srv):
    """a second scan of a strip (a rescan) numbers its windows from 1
    again: the earlier run's window files move to strip_rK_winNN first,
    so frames scanned before are not overwritten"""
    previewed(srv)
    d = srv.cwd / "strips" / "a"
    for k in (1, 2):
        code, t = srv.req("POST", "/api/tasks", {"op": "scan", "strip": "a"})
        assert code == 202 and srv.wait(t["id"])["exit"] == 0
    names = sorted(p.name for p in d.iterdir() if "win" in p.name)
    assert "strip_win01.raw" in names and "strip_r1_win01.raw" in names
    assert "strip_r1_win01.raw.txt" in names
    code, t = srv.req("POST", "/api/tasks", {"op": "scan", "strip": "a"})
    assert srv.wait(t["id"])["exit"] == 0
    assert (d / "strip_r2_win01.raw").exists()
