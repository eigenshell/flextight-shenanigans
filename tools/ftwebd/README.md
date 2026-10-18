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
(pending, confirmed, skipped, queued, scanning, done), the selected one
large as it will print (the preview printed with its clicks and turn;
once scanned, the scan's print), its name (the files' name; empty: the
strip's `<strip>_frameNN`), a crop (drag its edges or the box in the
large view, the strip around it shown), a turn, C/M/Y/D, Confirm and
Skip. When no frame is pending the scan starts by itself (3 s, Cancel or
Esc). A crop on a frame already queued, scanning or scanned asks:
Rescan (that frame in a run of its own), Keep old crop, Cancel. The page
is never locked by a task. Download (JPEG + `.txt`) and All (zip) for the
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

Strips live in `strips\<name>\`: `strip.raw` + `strip.png` + `strip.job`
from the preview, `strip.preview.job` (the preview's own job, crops
included: every plan starts from it), `strip_winNN.raw` (+ `.cal`,
`.txt`) per scanned window (an earlier run's as `strip_rK_winNN.raw`: a
rescan never overwrites them), `strip.scan` (the landing page's
choices), `strip.review` (each frame's decision) and `strip.print`
(emulsion, names, clicks and turns). Reloading the page restores the
strip from these files (the windows are cut again in the browser).

## Scan

Scan runs up to three steps in one task, each after the last one ends
well: the share windows (`ftpoc scan --job strip.job`, each window
focused first: a bracket on the first window, the rest carry its
offset on the curve, a 3-crop check where a window's scale is above
1.25 x the measured one. The status names the frame and the crop, that
frame pulses on the strip, and a small plot shows sharpness against the
offset and the offset taken: measured, checked or carried). The box
still takes `arc=` (frames as a detailed scan of their own into
`<archive>\<strip>\`: `ftpoc frames --detail neg --ppi 6300 --select
...`, then `ftpoc scan --job` on that job; refused without `--archive`),
but the page no longer offers it: scan to archive is its own scan type,
not yet built. Stop ends the step that runs and the chain.

## The page's files

`index.html` (< 14 KB: the shell renders from the first packet),
`review.js` (the review screen, deferred), `ftweb.js` (the worker) and
`ftweb.wasm` (~210 KB, ~90 KB gzip). They are sent with an ETag and
`no-cache`: the browser keeps them and asks only whether they changed.

## Not yet

Streamed frame detection (frames as the preview arrives); scanning from
the first confirmed frame on (now: when none is pending); per-frame
resolution; contrast, sharpening and NR per frame; recent scans; a vim
keymap; scan to archive; pushing JPEGs to the phone (Web Share) or a
NAS.

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
