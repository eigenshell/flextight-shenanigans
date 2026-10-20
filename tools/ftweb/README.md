# ftweb — the web page's engine

Everything the page does with files and pixels, in C on libft's browser
tier (heap and `snprintf`, no files, threads or clocks), compiled to
wasm32 for the browser. The page's JS only moves bytes between the box
(`ftwebd`), this engine and the DOM. The box scans and serves files; it
does no image work.

`ftweb.h` is the API (`ftw_*`): the box's focus fit, `strip.job` (the
preview's job as the base, the box's plan, re-plan a size per frame from
the base, describe with the frame boxes in `strip.png` pixels, write),
`strip.print` (load, emulsion, read and set clicks, write), cut a window
scan, measure the strip's balance, a frame's share JPEG and its text, a
screen copy (RGBA) and a zip of every print.

The page (`tools/ftwebd/www`) runs it in a worker: `ftweb.js` turns one
message into one call, `make www` puts `ftweb.wasm` beside it.

## Same bytes as the CLI

The engine runs `ftdev cut` then `ftdev print --roll` call for call
(`ft_share_cut`, `ft_share_print_frame`) and keeps each frame as the DNG
would keep it (16-bit samples, ppi and noise profile as stored, the
sidecar through its text). Each frame is balanced on its own (per-frame
balance, `ft_share_print_balance`: the preview a frame is judged on
predicts its share print), as `ftdev print --roll` balances one frame
given alone. So for the same windows, `strip.print` and clicks, the
page's JPEGs are the CLI's (one frame a call), byte for byte, and the JPEG
encoder is libft's own (`ft_jpeg.c`, itself byte-identical to
libjpeg-turbo with ftdev's settings). `tests/test_ftweb.py` checks both:
`ftweb_cli` (the engine driven from files) against ftdev, and the wasm
build against native.

## Build

```
make                    # ftweb_cli (native) + the browser-tier check
make wasm               # ftweb.wasm (the page's, ~210 KB) + ftweb_cli.wasm
make www                # ... and a copy in ../ftwebd/www for the page
```

`make wasm` needs the [wasi-sdk](https://github.com/WebAssembly/wasi-sdk)
(34; set `WASI_SDK`, default `~/.local/opt/wasi-sdk-34.0-x86_64-linux`).
Only libft's browser-tier units link into it. `ftweb.wasm` imports four
WASI calls from libc's own stdio and clock code (`fd_write`, `fd_close`,
`fd_seek`, `clock_time_get`); the page stubs them.

Run the WASI harness: `node wasi_run.mjs ftweb_cli.wasm <dir> --print
<dir>/strip.print -o <dir>/out <dir>/strip_win01.raw` (node 22 or later:
node 20's V8 crashes as the wasm memory grows).
