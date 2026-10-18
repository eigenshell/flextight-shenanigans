// SPDX-License-Identifier: GPL-2.0-or-later
// The page end to end in a headless Chromium (DevTools protocol over
// node's WebSocket; node >= 22): a user's taps against ftwebd + the fake
// ftpoc, the downloads saved to a folder. tests/test_page.py checks them.
//
//   node page_e2e.mjs <chromium> <http://127.0.0.1:port/> <download dir>
//
// Prints one JSON line: {strip, landed, notes, print, log}.
import { spawn } from "node:child_process";
import { mkdtempSync, readFileSync, existsSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";

const [chrome, url, dl] = process.argv.slice(2);
const prof = mkdtempSync(join(tmpdir(), "ftweb-e2e-"));
const proc = spawn(chrome, ["--headless=new", "--remote-debugging-port=0",
  "--user-data-dir=" + prof, "--no-first-run", "--no-default-browser-check",
  "--disable-gpu", "--window-size=1000,900", "about:blank"], { stdio: "ignore" });
const sleep = ms => new Promise(r => setTimeout(r, ms));
let ws, seq = 0;
const pending = new Map(), events = [];

async function connect() {
  const f = join(prof, "DevToolsActivePort");
  for (let i = 0; i < 200 && !existsSync(f); i++) await sleep(50);
  const [port, path] = readFileSync(f, "utf8").split("\n");
  ws = new WebSocket("ws://127.0.0.1:" + port + path);
  await new Promise((y, n) => { ws.onopen = y; ws.onerror = n; });
  ws.onmessage = m => {
    const d = JSON.parse(m.data);
    if (d.id && pending.has(d.id)) {
      const [y, n] = pending.get(d.id);
      pending.delete(d.id);
      d.error ? n(new Error(JSON.stringify(d.error))) : y(d.result);
    } else events.push(d);
  };
}
const cdp = (method, params = {}, sessionId) => new Promise((y, n) => {
  pending.set(++seq, [y, n]);
  ws.send(JSON.stringify({ id: seq, method, params, sessionId }));
});

let sid;
async function ev(expr) {
  const r = await cdp("Runtime.evaluate", { expression: expr, awaitPromise: true,
                                            returnByValue: true }, sid);
  if (r.exceptionDetails) throw new Error(expr + ": " + JSON.stringify(r.exceptionDetails));
  return r.result.value;
}
async function until(expr, what, ms = 60000) {
  for (let t = 0; t < ms; t += 100) {
    const v = await ev(expr);
    if (v) return v;
    await sleep(100);
  }
  const st = await ev('document.getElementById("st").textContent');
  const log = await ev('document.getElementById("log").textContent');
  throw new Error("timed out: " + what + " (status: " + st + ")\n" + log);
}

try {
  await connect();
  const { targetId } = await cdp("Target.createTarget", { url: "about:blank" });
  ({ sessionId: sid } = await cdp("Target.attachToTarget", { targetId, flatten: true }));
  await cdp("Browser.setDownloadBehavior", { behavior: "allow", downloadPath: dl,
                                             eventsEnabled: true });
  await cdp("Runtime.enable", {}, sid);
  await cdp("Page.enable", {}, sid);
  await cdp("Page.navigate", { url }, sid);
  await until('document.readyState === "complete" && typeof cut === "function"', "page");
  await until('document.getElementById("em").options.length > 0', "emulsions");
  // a new strip, previewed
  await ev('document.getElementById("new").click()');
  await until('document.getElementById("strip").value !== ""', "strip");
  const strip = await ev('document.getElementById("strip").value');
  await ev('document.getElementById("preview").click()');
  await until('document.querySelectorAll(".fr").length === 6', "frame boxes");
  // frame 3 -> 6MP, frame 4 -> ARC, frame 5 -> skip (each tap: next)
  const tap = (n, k) => ev('(()=>{const b=[...document.querySelectorAll(".fr")]' +
    '.find(b=>b.querySelector(".n").textContent==="' + n + '");' +
    'for(let i=0;i<' + k + ';i++)b.click()})()');
  await tap(3, 1);
  await tap(4, 2);
  await tap(5, 3);
  await until('[...document.querySelectorAll(".fr .s")].map(s=>s.textContent.split(" ")[0]).join() === "4MP,4MP,6MP,ARC,—,—"', "buckets");
  await until('/window/.test(document.getElementById("plan").textContent)', "plan");
  // each share frame's depth of field from the plan (ftw_dof95)
  await until('[...document.querySelectorAll(".fr .s")].filter(s=>/ DOF ±\\d+$/.test(s.textContent)).length === 3', "dof");
  const labels = await ev('[...document.querySelectorAll(".fr .s")].map(s=>s.textContent)');
  // frame 2 turned on the strip (its canvas shows the turn); the strip as
  // prints (pos): every frame's canvas, from the preview's raw
  const box = n => '[...document.querySelectorAll(".fr")].find(b=>b.querySelector(".n").textContent==="' + n + '")';
  await ev(box(2) + '.querySelector(".t").click()');
  await until(box(2) + '.querySelector("canvas").style.display === "block"', "strip turn");
  await until('fetch(S("strip.print")).then(r=>r.text()).then(t=>/\\[frame 2\\]\\nrotate = 90\\n/.test(t))', "strip turn saved");
  await until('!document.getElementById("pn").disabled', "pos available");
  await ev('document.getElementById("pn").click()');
  await until('[...document.querySelectorAll(".fr canvas")].every(c=>c.style.display==="block") && pvImg.size === 6', "pos");
  const posPx = await ev('(()=>{const c=' + box(1) + '.querySelector("canvas");return [...c.getContext("2d").getImageData(c.width/4|0,c.height/2|0,1,1).data]})()');
  // the status as the scan goes: focus crops, the offset taken (and the
  // plot's shaded half depth of field then)
  await ev('window.stSeen=[];new MutationObserver(()=>stSeen.push(document.getElementById("st").textContent+" |band "+(fzAt&&fzAt[2]))).observe(document.getElementById("st"),{childList:true,characterData:true,subtree:true})');
  await ev('document.getElementById("scan").click()');
  await until('/ftwebd exit 0/.test(document.getElementById("log").textContent)', "scan");
  await until('document.querySelectorAll("#grid canvas").length >= 2 && document.getElementById("big").width > 0', "prints");
  await ev('(async()=>{await landing})()');
  // clicks on the selected frame (the first): c c M (shift: minus)
  for (const key of ["c", "c", "M"])
    await ev('dispatchEvent(new KeyboardEvent("keydown",{key:"' + key + '"}))');
  await until('[...document.querySelectorAll("#keys b")].map(b=>b.textContent).join() === "+2,-1,0,0"', "clicks");
  // a quarter turn (r); the large view turns with it
  const big = 'document.getElementById("big")';
  const was = await ev('[' + big + '.width,' + big + '.height]');
  await ev('dispatchEvent(new KeyboardEvent("keydown",{key:"r"}))');
  await until(big + '.width === ' + was[1] + ' && ' + big + '.height === ' + was[0], "turned");
  // zoom in (z): one image pixel per device pixel, then a tap: fit again
  await ev('dispatchEvent(new KeyboardEvent("keydown",{key:"z"}))');
  await until(big + '.width > ' + was[1] + ' && document.getElementById("bw").className === "z"', "zoomed");
  await ev(big + '.click()');
  await until(big + '.width === ' + was[1] + ' && document.getElementById("bw").className === ""', "unzoomed");
  // the strip preview zooms
  await ev('document.getElementById("zi").click()');
  await until('document.getElementById("sv").style.minWidth === "960px"', "strip zoomed");
  await until('fetch(S("strip.print")).then(r=>r.text()).then(t=>/cmyd = 2 -1 0 0\\nrotate = 90\\n/.test(t))', "strip.print saved");
  await ev('document.getElementById("dl").click()');
  await ev('document.getElementById("all").click()');
  const want = [strip + "_frame01.jpg", strip + "_frame01.jpg.txt", strip + ".zip"];
  for (let t = 0; t < 600 && !want.every(f => existsSync(join(dl, f))); t++) await sleep(100);
  await sleep(300);
  const out = await ev('(async()=>({landed, posPx: ' + JSON.stringify(posPx) + ', seen: stSeen,' +
    ' plot: !document.getElementById("fz").hidden, labels: ' + JSON.stringify(labels) + ', notes: document.getElementById("notes").textContent,' +
    ' print: await fetch(S("strip.print")).then(r=>r.text()),' +
    ' log: document.getElementById("log").textContent}))()');
  console.log(JSON.stringify(Object.assign({ strip }, out)));
} catch (e) {
  console.error(String(e && e.stack || e));
  process.exitCode = 1;
} finally {
  try { ws && ws.close(); } catch (e) {}
  proc.kill();
  await sleep(200);
  rmSync(prof, { recursive: true, force: true });
}
