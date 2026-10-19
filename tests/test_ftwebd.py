# SPDX-License-Identifier: GPL-2.0-or-later
"""ftwebd, the scanner box's web server, against a fake ftpoc
(tests/fixtures/ftwebd/fake_ftpoc.py): the argv built per op, the event
stream and its resume, one task at a time, stop, the scan's chain (share
windows, then ARC as a detailed scan), the page's files (ETag) and the
checks on names and bodies (strip.job, strip.print)."""
import json
import os
import re
import resource
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
        """the task once it has ended (done, failed or stopped)"""
        for _ in range(500):
            code, t = self.req("GET", "/api/tasks/%d" % tid)
            if t and t["state"] not in ("queued", "running"):
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


@pytest.fixture
def held(binary, tmp_path):
    """scans wait while s.hold exists (fake_ftpoc.py FAKE_FTPOC_HOLD)"""
    hold = tmp_path / "hold"
    s = Server(binary, tmp_path, 0.001, env={"FAKE_FTPOC_HOLD": str(hold)})
    s.hold = hold
    yield s
    hold.unlink(missing_ok=True)
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


def only(text, keep):
    """a job's text with only the frames in keep included"""
    return re.sub(r"(\[frame (\d+)\]\ntype = \w+\ninclude = )yes",
                  lambda m: m.group(0) if int(m.group(2)) in keep
                  else m.group(1) + "no", text)


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
    """a scan's job is its own file, strip_j<id>.job (a copy of strip.job
    when the POST sends none); ftpoc names the outputs after it"""
    previewed(srv)
    code, t = srv.req("POST", "/api/tasks", {"op": "scan", "strip": "a"})
    assert code == 202 and t["id"] == 2
    assert srv.wait(t["id"])["exit"] == 0
    assert argv_log(srv)[-1] == ["scan", "--job", "strips/a/strip_j2.job",
                                 "--progress"]
    d = srv.cwd / "strips" / "a"
    assert (d / "strip_j2_win01.raw").exists()
    # ftpoc marks the finished windows done in the job it ran
    assert "done = yes" in (d / "strip_j2.job").read_text()
    assert "done = yes" not in (d / "strip.job").read_text()


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
    base = str(arc.cwd / "nas" / "a" / "strip_j2")
    assert argv_log(arc)[1:] == [
        ["scan", "--job", "strips/a/strip_j2.job", "--progress"],
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
    assert (arc.cwd / "nas" / "a" / "strip_j2_win01.raw").exists()
    # archive only: no share step
    code, t = arc.req("POST", "/api/tasks", {"op": "scan", "strip": "a",
                                             "share": "0", "arc": "2",
                                             "film": "slide"})
    assert code == 202 and arc.wait(t["id"])["exit"] == 0
    assert argv_log(arc)[-2][0:2] == ["frames", "--detail"]
    assert argv_log(arc)[-2][5:7] == ["--film", "slide"]
    assert argv_log(arc)[-2][-1] == str(arc.cwd / "nas" / "a" / "strip_j3")
    # job= is the share job's: without a share step it is not read
    code, t = arc.req("POST", "/api/tasks", {"op": "scan", "strip": "a",
                                             "share": "0", "arc": "2",
                                             "film": "neg", "job": "bogus"})
    assert code == 202 and arc.wait(t["id"])["exit"] == 0


def test_microcode_passed_to_every_step(binary, tmp_path):
    mc = tmp_path / "fw" / "MICROCOD.3XX"
    mc.parent.mkdir()
    mc.write_bytes(b"\0" * 16)
    s = Server(binary, tmp_path, 0.001,
               args=["--archive", tmp_path / "nas", "--microcode", mc])
    try:
        previewed(s)
        code, t = s.req("POST", "/api/tasks", {"op": "scan", "strip": "a",
                                               "arc": "4", "film": "neg"})
        assert code == 202 and s.wait(t["id"])["exit"] == 0
        log = argv_log(s)
        assert [a[0] for a in log] == ["frames", "scan", "frames", "scan"]
        for a in log:
            assert a[-2:] == ["--microcode", str(mc)]
    finally:
        s.close()


def test_microcode_must_exist(binary, tmp_path):
    r = subprocess.run([str(binary), "--port", str(free_port()), "--ftpoc",
                        str(FAKE), "--microcode", str(tmp_path / "none")],
                       cwd=tmp_path, capture_output=True, text=True,
                       timeout=10)
    assert r.returncode == 1
    assert "no microcode at" in r.stderr


def test_no_microcode_option_by_default(srv):
    previewed(srv)
    assert "--microcode" not in argv_log(srv)[0]


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


def test_stop_after_the_steps_output(binary, tmp_path):
    """a stop that lands once a step's output has ended (the step then
    exits 0): the job ends stopped and its next step never runs"""
    linger = tmp_path / "linger"
    s = Server(binary, tmp_path, 0.001, args=["--archive", tmp_path / "nas"],
               env={"FAKE_FTPOC_LINGER": str(linger)})
    try:
        previewed(s)
        linger.touch()
        code, t = s.req("POST", "/api/tasks", {"op": "scan", "strip": "a",
                                               "share": "1", "arc": "4",
                                               "film": "neg"})
        assert code == 202
        # the window written: the share step no longer heeds a stop and
        # soon lingers, its output closed
        w = tmp_path / "strips" / "a" / ("strip_j%d_win01.raw.txt" % t["id"])
        for _ in range(250):
            if w.exists():
                break
            time.sleep(0.02)
        time.sleep(0.2)
        assert s.req("DELETE", "/api/tasks/%d" % t["id"]) == \
            (200, {"ok": True, "was": "running"})
        linger.unlink()
        done = s.wait(t["id"])
        assert (done["state"], done["stopped"], done["exit"]) == \
            ("stopped", True, 0)
        assert [a[0] for a in argv_log(s)] == ["frames", "scan"]
        # stop on an ended job: 409, as before
        assert s.req("POST", "/api/tasks/%d/stop" % t["id"])[0] == 409
    finally:
        linger.unlink(missing_ok=True)
        s.close()


def test_jobs_run_in_order(held):
    """a POST while the scanner is busy is queued (no more 409): the
    jobs run one after the other in order; a job's lines stay readable
    after later jobs ran"""
    previewed(held)
    job = (held.cwd / "strips" / "a" / "strip.job").read_text()
    held.hold.touch()
    ids = []
    for keep in ({1}, {2}, {3}):
        code, t = held.req("POST", "/api/tasks", {"op": "scan", "strip": "a",
                                                  "job": only(job, keep)})
        assert code == 202
        ids.append(t["id"])
    assert ids == [2, 3, 4]
    assert [held.req("GET", "/api/tasks/%d" % i)[1]["state"] for i in ids] \
        == ["running", "queued", "queued"]
    assert held.req("GET", "/api/tasks/3")[1]["lines"] == 0
    assert held.req("GET", "/api/tasks/current")[1]["id"] == 2
    held.hold.unlink()
    for i in ids:
        assert held.wait(i)["exit"] == 0
    assert [a[2] for a in argv_log(held) if a[0] == "scan"] == \
        ["strips/a/strip_j%d.job" % i for i in ids]
    for i in ids:
        assert (held.cwd / "strips" / "a" / ("strip_j%d_win01.raw" % i)).exists()
    ev = held.events(2)
    assert [i for i, _ in ev] == list(range(1, len(ev) + 1))
    assert "strips/a/strip_j2.job" in ev[0][1]
    assert ev[-1][1] == "ftwebd exit 0"
    assert held.events(2, since=3) == ev[3:]


def test_ring_wraps_and_slots_recycle(binary, tmp_path):
    """more than 4096 lines and more than 8 jobs: an ended job's slot is
    recycled oldest first (gone: 404), and an ended job replays only the
    lines the shared ring still holds, from since=0 or an older since"""
    s = Server(binary, tmp_path, 0, env={"FAKE_FTPOC_ROWS": "40000"})
    try:
        previewed(s)
        job = only((s.cwd / "strips" / "a" / "strip.job").read_text(), {1})
        first, nlines = {}, s.req("GET", "/api/tasks/1")[1]["lines"]
        for want in range(2, 11):
            code, t = s.req("POST", "/api/tasks", {"op": "scan",
                                                   "strip": "a", "job": job})
            assert code == 202 and t["id"] == want
            done = s.wait(want)
            assert done["exit"] == 0
            first[want] = nlines + 1
            nlines += done["lines"]
        gone = nlines - 4096
        assert gone > first[4]                  # jobs 2-3 wholly overwritten
        # 9 jobs after the preview: jobs 1 and 2's slots went to 9 and 10
        # (no such job: null)
        assert s.req("GET", "/api/tasks/1") == (200, None)
        assert s.req("GET", "/api/tasks/2") == (200, None)
        assert s.req("GET", "/api/tasks/3")[1]["state"] == "done"
        assert s.events(3) == []                # its lines all overwritten
        k = max(t for t in first if first[t] <= gone)   # partly overwritten
        ev = s.events(k)
        assert ev[0][0] == gone - first[k] + 2
        assert [i for i, _ in ev] == list(range(ev[0][0], ev[-1][0] + 1))
        assert ev[-1][1] == "ftwebd exit 0"
        assert s.events(k, since=1) == ev
        ev = s.events(10)
        assert len(ev) == s.req("GET", "/api/tasks/10")[1]["lines"]
        assert "strips/a/strip_j10.job" in ev[0][1]
    finally:
        s.close()


def fds_free(pid, free):
    """an fd limit leaving `free` descriptors to the process pid: the
    number of its (free + 1)th unused one (fds above it stay open)"""
    fds = {int(f) for f in os.listdir("/proc/%d/fd" % pid)}
    n = 0
    while True:
        if n not in fds:
            if not free:
                return n
            free -= 1
        n += 1


@pytest.mark.skipif(not hasattr(resource, "prlimit") or os.geteuid() == 0,
                    reason="needs Linux prlimit, not as root")
def test_job_that_cannot_start(held):
    """a job whose first step cannot spawn (no fds for its pipe): queued,
    it ends at once (its error, exit -1) and its job file goes; on an
    idle box the POST is refused (422) and no slot is kept"""
    previewed(held)
    job = (held.cwd / "strips" / "a" / "strip.job").read_text()
    d = held.cwd / "strips" / "a"
    held.hold.touch()
    for want in (2, 3):
        code, t = held.req("POST", "/api/tasks", {"op": "scan", "strip": "a",
                                                  "job": job})
        assert code == 202 and t["id"] == want
    time.sleep(0.3)                             # connections closed
    pid = held.p.pid
    lim = resource.prlimit(pid, resource.RLIMIT_NOFILE)
    # job 2's pipe closes as it ends: one fd free, not the two of a pipe
    resource.prlimit(pid, resource.RLIMIT_NOFILE, (fds_free(pid, 0), lim[1]))
    held.hold.unlink()
    for _ in range(250):                        # no HTTP: accept has no fd
        if not (d / "strip_j3.job").exists():
            break
        time.sleep(0.02)
    time.sleep(0.1)
    resource.prlimit(pid, resource.RLIMIT_NOFILE, lim)
    assert held.wait(2)["exit"] == 0
    t = held.wait(3)
    assert t["exit"] == -1 and t["lines"] == 2
    ev = held.events(3)
    assert ev[0][1].startswith("pipe: ")
    assert ev[1][1] == "ftwebd exit -1"
    assert not (d / "strip_j3.job").exists()
    # idle: the POST is refused, no slot kept (a preview: no file to
    # write, so the request's only fds are its socket and the pipe's)
    time.sleep(0.3)
    resource.prlimit(pid, resource.RLIMIT_NOFILE, (fds_free(pid, 2), lim[1]))
    code, e = held.req("POST", "/api/tasks",
                       {"op": "preview", "strip": "a", "film": "neg"})
    resource.prlimit(pid, resource.RLIMIT_NOFILE, lim)
    assert code == 422 and e["error"].startswith("pipe: ")
    assert held.req("GET", "/api/tasks/4") == (200, None)
    assert held.req("GET", "/api/tasks/current")[1]["id"] == 3
    code, t = held.req("POST", "/api/tasks", {"op": "scan", "strip": "a",
                                              "job": job})
    assert code == 202 and t["id"] == 5 and held.wait(5)["exit"] == 0


def test_put_job_while_queued(held):
    """PUT /api/tasks/{id}/job replaces a queued job's file (checked like
    strip.job); once the job has started: 409, the file as ftpoc has it.
    One lock covers state and file: a PUT lands before the start or not
    at all"""
    previewed(held)
    d = held.cwd / "strips" / "a"
    job = (d / "strip.job").read_text()
    held.hold.touch()
    _, a = held.req("POST", "/api/tasks", {"op": "scan", "strip": "a",
                                           "job": only(job, {1})})
    _, b = held.req("POST", "/api/tasks", {"op": "scan", "strip": "a",
                                           "job": only(job, {2})})
    p = "/api/tasks/%d/job" % b["id"]
    assert held.req("PUT", p, only(job, {2, 3})) == (200, {"ok": True})
    assert (d / ("strip_j%d.job" % b["id"])).read_text() == only(job, {2, 3})
    code, e = held.req("PUT", p, "[strip]\nbogus = 1\n")
    assert code == 422 and "bogus" in e["error"]
    assert (d / ("strip_j%d.job" % b["id"])).read_text() == only(job, {2, 3})
    code, e = held.req("PUT", "/api/tasks/%d/job" % a["id"], only(job, {1, 2}))
    assert (code, e["error"]) == (409, "job %d has started" % a["id"])
    assert (d / ("strip_j%d.job" % a["id"])).read_text() == only(job, {1})
    assert held.req("PUT", "/api/tasks/99/job", job)[0] == 404
    assert held.req("PUT", "/api/tasks/1/job", job) == \
        (422, {"error": "job 1 has no share job"})
    held.hold.unlink()
    assert held.wait(b["id"])["exit"] == 0
    # ftpoc scanned what the PUT left: frames 2 and 3 done
    kept = (d / ("strip_j%d.job" % b["id"])).read_text()
    done = dict(re.findall(r"\[frame (\d)\][^\[]*?\ndone = (\w+)", kept))
    assert (done["1"], done["2"], done["3"]) == ("no", "yes", "yes")
    # ended: 409, as once started
    assert held.req("PUT", p, job) == \
        (409, {"error": "job %d has started" % b["id"]})
    assert (d / ("strip_j%d.job" % b["id"])).read_text() == kept
    assert not (d / ("strip_j%d.job.tmp" % b["id"])).exists()


def test_failed_job_skipped(binary, tmp_path):
    """a job that fails is marked failed; the next one runs"""
    hold = tmp_path / "hold"
    s = Server(binary, tmp_path, 0.001,
               env={"FAKE_FTPOC_HOLD": str(hold),
                    "FAKE_FTPOC_FAIL": "strip_j2.job"})
    try:
        previewed(s)
        hold.touch()
        _, a = s.req("POST", "/api/tasks", {"op": "scan", "strip": "a"})
        _, b = s.req("POST", "/api/tasks", {"op": "scan", "strip": "a"})
        assert (a["id"], b["id"]) == (2, 3)
        hold.unlink()
        t = s.wait(3)
        assert (t["state"], t["exit"]) == ("done", 0)
        t = s.req("GET", "/api/tasks/2")[1]
        assert (t["state"], t["exit"], t["stopped"]) == ("failed", 1, False)
        d = tmp_path / "strips" / "a"
        assert (d / "strip_j3_win01.raw").exists()
        assert not (d / "strip_j2_win01.raw").exists()
    finally:
        hold.unlink(missing_ok=True)
        s.close()


def test_delete_queued_and_running(held):
    """DELETE drops a queued job (and its job file) or stops the running
    one (marked stopped; the next runs); ?state=queued clears the queue"""
    previewed(held)
    d = held.cwd / "strips" / "a"
    held.hold.touch()
    ids = [held.req("POST", "/api/tasks", {"op": "scan", "strip": "a"})[1]["id"]
           for _ in range(4)]
    assert ids == [2, 3, 4, 5]
    assert held.req("DELETE", "/api/tasks/3") == \
        (200, {"ok": True, "was": "queued"})
    assert not (d / "strip_j3.job").exists()
    assert held.req("GET", "/api/tasks/3")[1] is None
    # job 2's fake ftpoc has started (it traps SIGINT before it logs)
    for _ in range(250):
        if ["scan", "--job", "strips/a/strip_j2.job"] in \
                [a[:3] for a in argv_log(held)]:
            break
        time.sleep(0.02)
    assert held.req("DELETE", "/api/tasks/2") == \
        (200, {"ok": True, "was": "running"})
    t = held.wait(2)
    assert (t["state"], t["stopped"]) == ("stopped", True)
    for _ in range(100):
        if held.req("GET", "/api/tasks/4")[1]["state"] == "running":
            break
        time.sleep(0.02)
    assert held.req("GET", "/api/tasks/4")[1]["state"] == "running"
    assert held.req("DELETE", "/api/tasks?state=queued") == \
        (200, {"dropped": 1})
    assert held.req("GET", "/api/tasks/5")[1] is None
    assert not (d / "strip_j5.job").exists()
    held.hold.unlink()
    assert held.wait(4)["state"] == "done"
    assert held.req("DELETE", "/api/tasks/2") == (409, {"error": "ended"})
    assert held.req("DELETE", "/api/tasks/99")[0] == 404
    assert held.req("DELETE", "/api/tasks?state=done")[0] == 422
    assert [a[2] for a in argv_log(held) if a[0] == "scan"] == \
        ["strips/a/strip_j2.job", "strips/a/strip_j4.job"]


def test_batch_full(held):
    """8 jobs queued or running: the next POST is refused with 507 (ended
    jobs' slots are recycled)"""
    previewed(held)
    held.hold.touch()
    for _ in range(8):
        assert held.req("POST", "/api/tasks", {"op": "scan", "strip": "a"})[0] \
            == 202
    assert held.req("GET", "/api/tasks/1")[1] is None   # the preview's slot
    assert held.req("POST", "/api/tasks", {"op": "scan", "strip": "a"}) == \
        (507, {"error": "the batch is full: 8 jobs queued or running"})
    assert held.req("DELETE", "/api/tasks?state=queued") == \
        (200, {"dropped": 7})
    held.hold.unlink()
    assert held.wait(2)["state"] == "done"


def batch_stream(s, secs=20, query="", headers=None):
    """the batch's snapshots (SSE /api/batch/events), as they come; the
    stream is open on return"""
    r = urllib.request.Request("http://127.0.0.1:%d/api/batch/events%s" %
                               (s.port, query), headers=headers or {})
    f = urllib.request.urlopen(r, timeout=secs)
    assert f.headers["Content-Type"] == "text/event-stream"
    end = time.time() + secs

    def snaps():
        with f:
            eid = None
            for line in f:
                assert time.time() < end, "no such snapshot in time"
                line = line.decode().rstrip("\n")
                if line.startswith("id: "):
                    eid = int(line[4:])
                elif line.startswith("data: "):
                    x = json.loads(line[6:])
                    assert eid == x["rev"]
                    yield x
    return snaps()


def first(snaps, ok):
    return next(x for x in snaps if ok(x))


def test_batch_events(held):
    """a snapshot at once, then one per change: queued, running, ended,
    dropped"""
    previewed(held)
    snaps = batch_stream(held)
    s0 = next(snaps)
    assert s0["jobs"] == [{"id": 1, "op": "preview", "strip": "a",
                           "state": "done", "exit": 0}]
    held.hold.touch()
    held.req("POST", "/api/tasks", {"op": "scan", "strip": "a"})
    held.req("POST", "/api/tasks", {"op": "scan", "strip": "a"})
    s1 = first(snaps, lambda x: [j["state"] for j in x["jobs"]] ==
               ["done", "running", "queued"])
    assert s1["rev"] > s0["rev"]
    held.req("DELETE", "/api/tasks/3")
    first(snaps, lambda x: [j["id"] for j in x["jobs"]] == [1, 2])
    held.hold.unlink()
    s3 = first(snaps, lambda x: x["jobs"][-1]["state"] == "done")
    assert s3["jobs"][-1] == {"id": 2, "op": "scan", "strip": "a",
                              "state": "done", "exit": 0}


def test_batch_events_stop_and_refusal(held):
    """a stop is a change (running, then stopped); so is a refused POST
    that took an ended job's slot (that job is gone)"""
    previewed(held)
    held.hold.touch()
    snaps = batch_stream(held)
    _, t = held.req("POST", "/api/tasks", {"op": "scan", "strip": "a"})
    first(snaps, lambda x: x["jobs"][-1] == {
        "id": t["id"], "op": "scan", "strip": "a", "state": "running",
        "exit": -1})
    assert held.req("POST", "/api/tasks/%d/stop" % t["id"]) == \
        (200, {"ok": True})
    first(snaps, lambda x: x["jobs"][-1]["state"] == "stopped")
    held.hold.unlink()
    for _ in range(6):                  # 8 ended jobs: every slot used
        _, t = held.req("POST", "/api/tasks", {"op": "scan", "strip": "a"})
        held.wait(t["id"])
    snaps = batch_stream(held, secs=5)
    assert [j["id"] for j in next(snaps)["jobs"]] == list(range(1, 9))
    held.req("POST", "/api/strips", {"name": "b"})     # no strip.job
    assert held.req("POST", "/api/tasks", {"op": "scan", "strip": "b"}) == \
        (422, {"error": "no strip.job to scan (send job=)"})
    assert [j["id"] for j in next(snaps)["jobs"]] == list(range(2, 9))


def test_batch_events_resume(held):
    """since= or Last-Event-ID: the current snapshot at once if the batch
    has changed since, else the next change's (a PUT and a clear count)"""
    previewed(held)
    rev = next(batch_stream(held))["rev"]
    held.hold.touch()
    _, a = held.req("POST", "/api/tasks", {"op": "scan", "strip": "a"})
    _, b = held.req("POST", "/api/tasks", {"op": "scan", "strip": "a"})
    for kw in ({"query": "?since=%d" % rev},
               {"headers": {"Last-Event-ID": str(rev)}}):
        s = next(batch_stream(held, **kw))
        assert s["rev"] > rev
        assert [j["state"] for j in s["jobs"]] == ["done", "running",
                                                   "queued"]
    now = next(batch_stream(held))["rev"]
    snaps = batch_stream(held, query="?since=%d" % now)
    job = (held.cwd / "strips" / "a" / "strip.job").read_text()
    assert held.req("PUT", "/api/tasks/%d/job" % b["id"], job) == \
        (200, {"ok": True})
    s = next(snaps)
    assert s["rev"] == now + 1
    assert held.req("DELETE", "/api/tasks?state=queued") == \
        (200, {"dropped": 1})
    s = next(snaps)
    assert s["rev"] == now + 2
    assert [j["id"] for j in s["jobs"]] == [1, a["id"]]
    held.hold.unlink()


def test_stop(slow):
    slow.req("POST", "/api/strips", {"name": "a"})
    code, t = slow.req("POST", "/api/tasks",
                       {"op": "preview", "strip": "a", "film": "slide"})
    assert code == 202
    time.sleep(0.3)
    assert slow.req("POST", "/api/tasks/%d/stop" % t["id"]) == \
        (200, {"ok": True})
    done = slow.wait(t["id"])
    assert done["stopped"] and done["exit"] == 1
    assert done["state"] == "stopped"
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
                                   "batch.c", "proc_posix.c")]
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


def test_scan_job_per_task(srv):
    """a rescan is a later job: its own job file and windows, nothing of
    an earlier scan renamed or overwritten"""
    previewed(srv)
    d = srv.cwd / "strips" / "a"
    for k in (1, 2, 3):
        code, t = srv.req("POST", "/api/tasks", {"op": "scan", "strip": "a"})
        assert code == 202 and srv.wait(t["id"])["exit"] == 0
    names = sorted(p.name for p in d.iterdir() if "win" in p.name)
    assert [n for n in names if n.endswith(".raw")] == \
        ["strip_j2_win01.raw", "strip_j3_win01.raw", "strip_j4_win01.raw"]
    assert not [n for n in names if "_r1_" in n]


def test_scan_job_from_the_post(srv):
    """job= on the POST: checked like a PUT strip.job, kept as
    strip_j<id>.job, the job ftpoc scans"""
    previewed(srv)
    d = srv.cwd / "strips" / "a"
    job = only((d / "strip.job").read_text(), {2, 3})
    code, t = srv.req("POST", "/api/tasks",
                      {"op": "scan", "strip": "a", "job": job})
    assert code == 202 and srv.wait(t["id"])["exit"] == 0
    kept = (d / ("strip_j%d.job" % t["id"])).read_text()
    assert re.findall(r"include = (\w+)", kept) == \
        re.findall(r"include = (\w+)", job)
    code, e = srv.req("POST", "/api/tasks", {"op": "scan", "strip": "a",
                                             "job": "[strip]\nbogus = 1\n"})
    assert code == 422 and e["error"].startswith("job: ")
    assert "bogus" in e["error"]
    # a job= that does not decode (a NUL, a bad %XX) is refused, never
    # taken for "no job=" (a copy of strip.job)
    for bad in ("%00", "%zz", "[strip]%00x"):
        code, e = srv.req("POST", "/api/tasks",
                          "op=scan&strip=a&job=" + bad)
        assert code == 422 and e["error"].startswith("job: "), bad
    assert len(argv_log(srv)) == 2


def test_scan_needs_a_job(srv):
    srv.req("POST", "/api/strips", {"name": "a"})
    assert srv.req("POST", "/api/tasks", {"op": "scan", "strip": "a"}) == \
        (422, {"error": "no strip.job to scan (send job=)"})
    assert not (srv.cwd / "fake_ftpoc.log").exists()
