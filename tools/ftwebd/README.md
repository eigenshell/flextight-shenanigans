# ftwebd — the scanner box's web server

A landing page and a review screen (spec: the 2026-10-18 landing /
review design). The landing page: quick scan (scan to archive: not yet),
a resolution per strip in ppi with the 36x24 frame's MP (Dmax: #N/A, no
estimator yet), the holder, the emulsion, the output (local storage;
network and cloud not yet), recent scans (not yet) and SCAN: a new strip
(`YYYY-MM-DD_a`, the choices kept as `strip.scan`) and its preview.

The review screen: the strip bar fills as the preview scans (the raw
lines read with byte ranges, drawn by the engine; rough colour, no
calibration), then each frame on its own: thumbnails with a state
(pending, confirmed, skipped, queued, scanning, done, retry), the selected one
large as it will print (the preview printed with its clicks and turn;
once scanned, the scan's print), its name (the files' name; empty: the
strip's `<strip>_frameNN`), a crop (drag its edges or the box in the
large view, the strip around it shown), a turn, C/M/Y/D, Confirm and
Skip. Confirmed frames go to the box's batch 3 s after the last decision
(each decision restarts the countdown; Cancel or Esc holds them back):
the first confirm starts a scan, and frames confirmed while it runs
collect into the next job, which runs right after it. Backspace (or
Skip) on a queued frame takes it out of its job (the job re-planned, or
dropped with its last frame); a frame in the job that is scanning cannot
be taken out (Skip is disabled; Stop ends that job). Frames of a job
that failed, was stopped or cleared, or whose window did not hold them,
show "retry": Confirm sends them again, as a job of their own. Beside
the status: "Scanning · N frames left" (nothing when idle; jobs are
never shown), and Clear queue while a job waits (it drops every queued
job on the box). A crop on a frame already queued, scanning or scanned
asks: Rescan (that frame in a job of its own), Keep old crop, Cancel.
Reloading the page (or another tab) rebuilds the batch from the box: its
snapshot plus each job's file; a box restart loses it (the frames show
as not scanned). The page is never locked by a task. Download (JPEG + `.txt`) and All (zip) for the
scanned frames; `1:1` (or the zoom key) shows the print one image pixel
per screen pixel.

Keys, as presets matched on the typed character (`?` or `/` shows them
and picks the preset; kept per browser):

| | qwerty | dvorak |
|---|---|---|
| D + / − | Q / A | ' / A |
| C + / − | W / S | , / O |
| M + / − | E / D | . / E |
| Y + / − | R / F | P / U |
| turn left / right | T / G | Y / I |
| name, zoom | N, Z | B, F |

Every preset: ← → frames, Home End, Enter confirm, X skip, Backspace back
to pending, 0 reset C/M/Y/D, Esc.

A window keeps one pixel count (the strip's ppi on a 36 mm long side):
a cropped frame needs more ppi for it, and its run scans at that; the
page says so under the job list. Partial and half frames would raise it
for their whole run: they show "under construction" (skipped, Confirm
refused) until per-frame resolution.

The box does no image or planning work. The page does, in C compiled to
wasm (`tools/ftweb`, `ftweb.wasm` in a worker): it plans the share
windows, cuts each window scan as it lands, measures each frame's colour
balance and prints, with the same bytes as `ftdev cut` + `ftdev print
--roll` (one frame a call). The server runs whitelisted `ftpoc` tasks and streams their
output (the client never sends a command line), and keeps the strip's
files, checking `strip.job` and `strip.print` with libft before it writes
them.

## Run it (Windows scanner box)

```
build.bat                      REM winlibs mingw (posix threads) on PATH
REM on a PC with the wasi-sdk: make -C tools/ftweb www  -> www\ftweb.wasm
copy ftwebd.exe, ..\poc\ftpoc.exe and www\ (index.html, review.js,
ftweb.js, ftweb.wasm) into one folder:
  web\ftwebd.exe  web\ftpoc.exe  web\www\...
cd web
ftwebd --archive D:\scans       REM --port 8080 --strips strips --www www
                                REM --ftpoc ftpoc.exe --archive-ppi 6300
                                REM --microcode ..\poc\MICROCOD.3XX
```

Then open `http://<box>:8080/` from a phone or PC on the LAN (Windows
Firewall: allow TCP 8080 from the local subnet). Start it from a console
window (Stop sends Ctrl+Break to ftpoc's process group, which needs one),
from ftpoc's folder: ftpoc reads `focus.txt` there, and the page's
planner gets it from ftwebd. After a power-up ftpoc uploads the
scanner's microcode by itself before the task (the status says so):
put `MICROCOD.3XX` beside `ftpoc.exe`, or start ftwebd with
`--microcode PATH` (e.g. `--microcode ..\poc\MICROCOD.3XX`), which it
passes to every ftpoc task.

Strips live in `strips\<name>\`: `strip.raw` (+ `.cal`, written first,
and `.txt`) and `strip.png` from the preview, `strip.preview.job` (the
box's own detection of the preview, ft_detect: kept beside the page's for
comparison, not used for frames), `strip.job` (the page's: the frames its
streaming detector found as the preview arrived, crops included, as
planned; written when the preview ends, or stops with a frame, and on a
crop or a scan),
`strip_j<id>.job` per scan job (the job the page planned; ftpoc marks its
windows done in it) and `strip_j<id>_winNN.raw` (+ `.cal`, `.txt`) per
scanned window (ftpoc names them after the job, so a rescan, a later
job, never overwrites them; strips from before keep their `strip_winNN`
/ `strip_rK_winNN` names), `strip.scan` (the landing page's
choices), `strip.review` (each frame's decision) and `strip.print`
(emulsion, names, clicks and turns). Reloading the page restores the
strip from these files: the preview replayed through the streaming
detector gives the same frames, the edits re-attach by frame number, the
windows are cut again in the browser.
Every file the box writes (a PUT, a job file, a kept preview job) goes
to `<file>.tmp` and is renamed over it, under one lock: a GET racing a
write gets the old file or the new one, whole.

## Preview

The page shows each frame as soon as the preview has passed the gap after
it: the worker calibrates the raw lines the live bar reads
(`strip.raw.cal` is written before them) and feeds them to the streaming
detector (`libft/ft_stream_detect`); a sealed frame prints with its own
balance and can be named, turned, nudged, cropped, confirmed or skipped
at once; it stays as shown (later lines only add frames). Frames
confirmed while the preview runs go to the batch once it has ended.

## Scan

Scan runs up to three steps in one task, each after the last one ends
well: the share windows (`ftpoc scan --job strip_j<id>.job`, each window
focused first: a bracket on the first window, the rest carry its
offset on the curve, a 3-crop check where a window's scale is above
1.25 x the measured one. The status names the frame and the crop, that
frame pulses on the strip, and a small plot shows sharpness against the
offset and the offset taken: measured, checked or carried). The box
still takes `arc=` (frames as a detailed scan of their own into
`<archive>\<strip>\` as `strip_j<id>`: `ftpoc frames --detail neg --ppi 6300 --select
...`, then `ftpoc scan --job` on that job; refused without `--archive`),
but the page no longer offers it: scan to archive is its own scan type,
not yet built. Stop ends the step that runs and the chain.

Scans and previews are jobs of the box's batch (8 slots; 8 at most
queued or running, ended jobs' slots reused oldest first): they run one
after the other in the order queued; one that fails or is stopped is
marked so and the next runs. The page follows each job by its own event
stream, from its first line to its `ftwebd exit` line, so a job's last
window lands even when the next has started (the box keeps one ring of
lines across jobs: an ended job's lines stay until newer ones overwrite
them). ftpoc children inherit none of ftwebd's sockets (close-on-exec;
on Windows not inheritable), so a connection ends when ftwebd closes it,
not when the scan it started ends.

## The page's files

`index.html` (< 14 KB: the shell renders from the first packet),
`review.js` (the review screen, deferred), `ftweb.js` (the worker) and
`ftweb.wasm` (~210 KB, ~90 KB gzip). They are sent with an ETag and
`no-cache`: the browser keeps them and asks only whether they changed.

## Not yet

Per-frame resolution; contrast, sharpening and NR per frame; recent scans; a vim
keymap; scan to archive; pushing JPEGs to the phone (Web Share) or a
NAS.

## API

LAN, no auth; JSON answers (errors: `{"error": "..."}`) unless said.
Form bodies are `application/x-www-form-urlencoded`.

| | |
|---|---|
| `GET /`, `/index.html`, `/<file>` | the page's files (from `--www`; ETag, 304 while unchanged) |
| `GET /api/focus.txt` | this scanner's focus fit; 404: none (the built-in curve) |
| `GET /api/strips` | `[{name, job, png, print}]` |
| `POST /api/strips` `name=[&type=quick&ppi=&holder=&emulsion=]` | the folder created; with the landing page's choices, kept as `strip.scan` (+ `created =`); 422 on a bad name or choice |
| `GET /api/scans` | recent scans: `[]` (to come) |
| `GET /api/strips/{s}/files` | `[{name, size}]` |
| `GET /api/strips/{s}/files/{f}` | one file; `Range: bytes=N-`: 206 with what is there, 416 nothing new (the live bar). `strip_j<id>.job`: a job's frames (the page rebuilds the batch from them after a reload) |
| `GET\|PUT /api/strips/{s}/strip.job`, `strip.print`, `strip.preview.job` | the whole file; PUT checked with libft (422) |
| `GET\|PUT /api/strips/{s}/strip.review` | `<frame> pending\|confirmed\|skipped` lines; PUT checked |
| `POST /api/tasks` `op=preview&strip=&film=neg\|slide` | 202 `{id}`: a job queued at the batch's tail (it starts at once when idle); 404 no such strip; 422 a bad request; 507 when 8 jobs are queued or running |
| `POST /api/tasks` `op=scan&strip=&share=1&job=<text>[&arc=1,3-4&film=]` | the same; `job` is the share job, checked and kept as `strip_j<id>.job` (absent: a copy of `strip.job`); 422 on a bad one |
| `PUT /api/tasks/{id}/job` | a queued scan's job replaced (checked; body: the job's text): 200; 409 once it has started (one lock: the PUT lands before the start or not at all); 422 bad, or a preview job; 404 none |
| `DELETE /api/tasks/{id}` | 200 `{"ok":true,"was":"queued"}`: dropped (its job file too), or `"was":"running"`: stopped (it ends `stopped`, the next runs); 409 ended; 404 none |
| `DELETE /api/tasks?state=queued` | 200 `{"dropped":n}`: every queued job dropped, box-wide |
| `POST /api/tasks/{id}/stop` | the running job stopped (as DELETE on it); 409 not running; 404 none |
| `GET /api/tasks/{id}`, `/api/tasks/current` | `{id, op, strip, state, exit, stopped, lines}`, state `queued\|running\|done\|failed\|stopped`; current: the running job, else the latest; `null` when none |
| `GET /api/tasks/{id}/events?since=k` | SSE, a job's lines, `id:` its line number (1: the command; the last: `ftwebd exit <code>`), from `since` or `Last-Event-ID`; a queued job's stream waits for its start; `event: end` after its last line, `event: gone` when it was dropped or its slot reused (lines already overwritten are skipped) |
| `GET /api/batch/events?since=rev` | SSE, `id:` the revision: `{"rev", "jobs": [{id, op, strip, state, exit}]}` at once (unless `since` or `Last-Event-ID` is the current revision) and on every change |

## Linux

`make` builds `ftwebd` (POSIX); `tests/test_ftwebd.py` drives it against
`tests/fixtures/ftwebd/fake_ftpoc.py`, and `tests/test_page.py` drives
the page in a headless Chromium. A local demo with real (synthetic)
pixels:

```
make -C tools/ftweb www
make -C libft test/window_dump && libft/test/window_dump /tmp/win
FAKE_FTPOC_WINDOW=/tmp/win ftwebd --ftpoc tests/fixtures/ftwebd/fake_ftpoc.py \
    --www tools/ftwebd/www --archive /tmp/archive
```
