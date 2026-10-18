// SPDX-License-Identifier: GPL-2.0-or-later
// The page's worker: ftweb.wasm (tools/ftweb, the engine in C) behind one
// message per call. Bytes in and out only; the work is the engine's.
// Message {id, op, a: [args]} -> {id, r} or {id, err}.
"use strict";
let X, M, w;
const enc = new TextEncoder(), dec = new TextDecoder();
const u8 = () => new Uint8Array(M.buffer);
// libc's own WASI calls (stdio flushed at exit, the clock): stubbed
const wasi = {
  fd_write(fd, iov, n, nw) {
    const v = new DataView(M.buffer);
    let k = 0;
    for (let i = 0; i < n; i++) k += v.getUint32(iov + i * 8 + 4, true);
    v.setUint32(nw, k, true);
    return 0;
  },
  fd_close: () => 0,
  fd_seek: () => 70,
  clock_time_get(id, prec, out) {
    new DataView(M.buffer).setBigUint64(out, BigInt(Date.now()) * 1000000n, true);
    return 0;
  },
};
const ready = (async () => {
  const r = await WebAssembly.instantiateStreaming(fetch("ftweb.wasm"),
                                                   { wasi_snapshot_preview1: wasi });
  X = r.instance.exports;
  M = X.memory;
  if (X._initialize) X._initialize();
  w = X.ftw_new();
})();

// bytes (or a string) into the engine's heap; free with X.ftw_release
function put(b, z) {
  if (typeof b === "string") b = enc.encode(b);
  const p = X.ftw_alloc(b.length + (z ? 1 : 0));
  u8().set(b, p);
  if (z) u8()[p + b.length] = 0;
  return p;
}
function cstr(p) {
  const m = u8();
  let e = p;
  while (m[e]) e++;
  return dec.decode(m.subarray(p, e));
}
function ok(rc) {
  if (rc < 0) throw new Error(cstr(X.ftw_error(w)));
  return rc;
}
const out = () => u8().slice(X.ftw_out(w), X.ftw_out(w) + X.ftw_out_len(w));
const text = () => dec.decode(out());
// f(ptrs...) with each argument put in the heap, released after
function withPut(args, f) {
  const ps = args.map(a => (a == null ? 0 : put(a, typeof a === "string")));
  try {
    return f(...ps);
  } finally {
    ps.forEach(p => p && X.ftw_release(p));
  }
}
const load = fn => t => (withPut([t], p => ok(fn(w, p, enc.encode(t).length))), null);
function ints(n, f) {
  const p = X.ftw_alloc(n * 4);
  try {
    const k = f(p);
    return Array.from(new Int32Array(M.buffer, p, n)).slice(0, k < 0 ? 0 : Math.min(k, n));
  } finally {
    X.ftw_release(p);
  }
}

// fn(w, frame, edge, &w, &h) -> {w, h, rgba}
function rgbaOf(fn, f, edge) {
  const p = X.ftw_alloc(8);
  try {
    ok(fn(w, f, edge, p, p + 4));
    const d = new Int32Array(M.buffer, p, 2);
    return { w: d[0], h: d[1], rgba: out() };
  } finally {
    X.ftw_release(p);
  }
}

const ops = {
  emulsions: () => (ok(X.ftw_emulsions(w)), JSON.parse(text())),
  // ppis -> [{ppi, mp, px, dmax}]
  resolutions(ppis) {
    const p = X.ftw_alloc(ppis.length * 8);
    try {
      new Float64Array(M.buffer, p, ppis.length).set(ppis);
      ok(X.ftw_resolutions(w, p, ppis.length));
      return JSON.parse(text());
    } finally {
      X.ftw_release(p);
    }
  },
  holders: () => (ok(X.ftw_holders(w)), JSON.parse(text())),
  lens: t => load(X.ftw_lens)(t),
  job: t => load(X.ftw_job_load)(t),
  planLoad: t => load(X.ftw_plan_load)(t),
  // sizes: one px per frame of the preview's job (0: skip)
  plan(sizes) {
    const p = X.ftw_alloc(sizes.length * 8);
    try {
      new Float64Array(M.buffer, p, sizes.length).set(sizes);
      const n = ok(X.ftw_plan(w, p, sizes.length));
      ok(X.ftw_job_text(w));
      return { windows: n, text: text() };
    } finally {
      X.ftw_release(p);
    }
  },
  jobJson: t => withPut([t || null], p => (ok(X.ftw_job_json(w, p, t ? enc.encode(t).length : 0)), JSON.parse(text()))),
  // a crop: box [x, y, w, h] in the preview's pixels -> the base job's text
  frameBox(f, box, meta) {
    const p = X.ftw_alloc(32);
    try {
      new Float64Array(M.buffer, p, 4).set(box);
      return withPut([meta || null], pm => (ok(X.ftw_frame_box(w, f, p, pm, meta ? enc.encode(meta).length : 0)), text()));
    } finally {
      X.ftw_release(p);
    }
  },
  printLoad: t => load(X.ftw_print_load)(t),
  printNew: id => withPut([id], p => (ok(X.ftw_print_new(w, p)), null)),
  printEmulsion: id => withPut([id], p => ok(X.ftw_print_emulsion(w, p))),
  printGet: f => ints(4, p => (ok(X.ftw_print_get(w, f, p)), 4)),
  clicks: (f, k) => (ok(X.ftw_print_clicks(w, f, k[0], k[1], k[2], k[3])), null),
  rotate: (f, deg) => (ok(X.ftw_print_rotate(w, f, deg)), null),
  rotation: f => ok(X.ftw_print_rotation(w, f)),
  name: f => (ok(X.ftw_print_name(w, f)), text()),
  setName: (f, n) => withPut([n], p => (ok(X.ftw_print_set_name(w, f, p)), null)),
  printText: () => (ok(X.ftw_print_text(w)), text()),
  // one window: its raw, .cal, .txt and .faul (or null) as bytes
  cut(raw, cal, meta, faul) {
    return withPut([raw, cal, faul, meta], (pr, pc, pf, pm) => {
      ok(X.ftw_cut(w, pr, raw.length, pc, cal.length, pf, faul ? faul.length : 0,
                   pm, meta.length, 0, 0));
      return JSON.parse(text());
    });
  },
  frames: () => ints(256, p => X.ftw_frames(w, p, 256)),
  // the preview's strip.raw, .cal, .txt and .faul (or null): its frames
  previewLoad(raw, cal, meta, faul) {
    return withPut([raw, cal, faul, meta], (pr, pc, pf, pm) =>
      ok(X.ftw_preview_load(w, pr, raw.length, pc, cal.length, pf,
                            faul ? faul.length : 0, pm, meta.length)));
  },
  measure: () => (ok(X.ftw_measure(w)), text()),
  display: (f, edge) => rgbaOf(X.ftw_display, f, edge),
  previewDisplay: (f, edge) => rgbaOf(X.ftw_preview_display, f, edge),
  previewPrint: (f, edge) => rgbaOf(X.ftw_preview_print, f, edge),
  previewPx: () => X.ftw_preview_px(),
  // raw preview lines (bytes, whole lines) -> {w, h, rgba} for the live bar
  liveRows(raw, px, neg, first) {
    return withPut([raw], p => {
      const h = ok(X.ftw_live_rows(w, p, raw.length >> 1, px, neg ? 1 : 0, first ? 1 : 0));
      return { w: px, h, rgba: out() };
    });
  },
  jpeg: f => (ok(X.ftw_jpeg(w, f)), { jpg: out(), text: cstr(X.ftw_text(w)) }),
  zip: base => withPut([base], p => (ok(X.ftw_zip(w, p)), out())),
};

// one call at a time, in order
let queue = ready;
self.onmessage = e => {
  const { id, op, a } = e.data;
  queue = queue.then(() => {
    const r = ops[op](...(a || []));
    const t = r && (r.rgba || r.jpg || (r instanceof Uint8Array ? r : null));
    self.postMessage({ id, r }, t ? [t.buffer] : []);
  }).catch(err => self.postMessage({ id, err: String(err && err.message || err) }));
};
