// SPDX-License-Identifier: GPL-2.0-or-later
// The page's worker (tools/ftwebd/www/ftweb.js) under node (>= 22): its
// self/fetch stand-ins, then the calls of a JSON file, in order, as the
// page sends them. tests/test_page.py.
//
//   node worker_run.mjs <www dir> <calls.json>
//
// calls: [{"op", "a": [...], "out": path?}]; an argument {"file": p} is
// that file's bytes. Prints [{"r"} | {"err"}] per call; byte results go
// to "out" and print as their length.
import { readFileSync, writeFileSync } from "node:fs";
import { join } from "node:path";
import vm from "node:vm";

const [www, callsPath] = process.argv.slice(2);
const replies = new Map();
globalThis.self = globalThis;
globalThis.postMessage = m => replies.get(m.id)(m);
globalThis.fetch = async () => new Response(readFileSync(join(www, "ftweb.wasm")),
                                            { headers: { "content-type": "application/wasm" } });
vm.runInThisContext(readFileSync(join(www, "ftweb.js"), "utf8"), { filename: "ftweb.js" });

const arg = a => (a && typeof a === "object" && !Array.isArray(a) && a.file
                  ? new Uint8Array(readFileSync(a.file)) : a);
const out = [];
let id = 0;
for (const c of JSON.parse(readFileSync(callsPath, "utf8"))) {
  const m = await new Promise(y => {
    replies.set(++id, y);
    self.onmessage({ data: { id, op: c.op, a: (c.a || []).map(arg) } });
  });
  if (m.err != null) {
    out.push({ err: m.err });
    continue;
  }
  let r = m.r;
  const bytes = r instanceof Uint8Array ? r : r && (r.jpg || r.rgba);
  if (bytes && c.out) {
    writeFileSync(c.out, bytes);
    if (r.jpg) writeFileSync(c.out + ".txt", r.text);
    r = r instanceof Uint8Array ? bytes.length : Object.assign({}, r, { jpg: undefined, rgba: undefined, bytes: bytes.length });
  }
  out.push({ r });
}
console.log(JSON.stringify(out));
