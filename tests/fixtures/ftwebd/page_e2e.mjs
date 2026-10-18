// SPDX-License-Identifier: GPL-2.0-or-later
// The page end to end in a headless Chromium (DevTools protocol over
// node's WebSocket; node >= 22): a user's choices, keys and drags against
// ftwebd + the fake ftpoc, the downloads saved to a folder.
// tests/test_page.py checks them.
//
//   node page_e2e.mjs <chromium> <http://127.0.0.1:port/> <download dir>
//
// Prints one JSON line: {strip, res, live, landed, ...}.
import { spawn } from "node:child_process";
import { mkdtempSync, readFileSync, existsSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";

const [chrome, url, dl] = process.argv.slice(2);
const prof = mkdtempSync(join(tmpdir(), "ftweb-e2e-"));
const proc = spawn(chrome, ["--headless=new", "--remote-debugging-port=0",
  "--user-data-dir=" + prof, "--no-first-run", "--no-default-browser-check",
  "--disable-gpu", "--window-size=1100,900", "about:blank"], { stdio: "ignore" });
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
  const st = await ev('(document.getElementById("rst")||document.getElementById("st")).textContent');
  const log = await ev('(document.getElementById("log")||{}).textContent');
  throw new Error("timed out: " + what + " (status: " + st + ")\n" + log);
}
const key = k => ev('dispatchEvent(new KeyboardEvent("keydown",{key:' + JSON.stringify(k) + '}))');
const mouse = (type, x, y) => cdp("Input.dispatchMouseEvent", { type, x, y, button: "left",
  buttons: type === "mouseReleased" ? 0 : 1, clickCount: 1 }, sid);
// drag the selected frame's right edge left by dx css px in the large view
async function dragRight(dx) {
  const [x, y] = await ev('(()=>{const S=toScreen(frame(sel)),c=document.getElementById("big"),' +
    'r=c.getBoundingClientRect(),k=r.width/c.width;return [r.left+(S.x+S.w)*k,r.top+(S.y+S.h/2)*k]})()');
  await mouse("mousePressed", x, y);
  for (let i = 1; i <= 5; i++) await mouse("mouseMoved", x - dx * i / 5, y);
  await mouse("mouseReleased", x - dx, y);
}
const fileHas = (f, re) => 'fetch(S(' + JSON.stringify(f) + ')).then(r=>r.text()).then(t=>' + re + '.test(t))';

try {
  await connect();
  const { targetId } = await cdp("Target.createTarget", { url: "about:blank" });
  ({ sessionId: sid } = await cdp("Target.attachToTarget", { targetId, flatten: true }));
  await cdp("Browser.setDownloadBehavior", { behavior: "allow", downloadPath: dl,
                                             eventsEnabled: true });
  await cdp("Runtime.enable", {}, sid);
  await cdp("Page.enable", {}, sid);
  await cdp("Page.navigate", { url }, sid);
  // the landing page: the engine's resolutions, holder, emulsions
  await until('document.querySelectorAll("#res .res").length === 6 && document.getElementById("em").options.length > 0', "landing");
  const res = await ev('[...document.querySelectorAll("#res .res")].map(b=>b.textContent)');
  const holder = await ev('document.getElementById("holder").selectedOptions[0].textContent');
  const recent = await ev('document.getElementById("recent").textContent');
  await ev('document.querySelector("#res .res[data-ppi=\\"2450\\"]").click()');
  await ev('document.getElementById("scan").click()');
  await until('location.hash.startsWith("#s=")', "review");
  const strip = await ev("cur");
  // the live bar fills while the preview scans, then the frames
  await until('typeof lv === "object" && lv.line > 0', "live rows", 30000);
  await until('document.querySelectorAll(".th").length === 6 && sel === 1 && document.getElementById("big").width > 0', "frames");
  const scanFile = await ev('fetch(S("files/strip.scan")).then(r=>r.text())');
  // frame 1 (qwerty): C + C + M - (w w d), turn right (g), confirm (Enter)
  for (const k of ["w", "w", "d"]) await key(k);
  await until('[...document.querySelectorAll("#keys b")].map(b=>b.textContent).join() === "+2,-1,0,0"', "clicks");
  await key("g");
  await until(fileHas("strip.print", '/\\[frame 1\\]\\ncmyd = 2 -1 0 0\\nrotate = 90\\n/'), "frame 1 saved");
  await key("Enter");
  await until("sel === 2", "next frame");
  // frame 2 (dvorak): turn right (i), D + (') and a name, confirm
  await ev('localStorage.setItem("keys","dvorak")');
  await key("i");
  await key("'");
  await until('[...document.querySelectorAll("#keys b")].map(b=>b.textContent).join() === "0,0,0,+1"', "dvorak D+");
  await ev('(()=>{const n=document.getElementById("name");n.value="dog";n.dispatchEvent(new Event("change"))})()');
  await until(fileHas("strip.print", '/\\[frame 2\\]\\nname = dog\\ncmyd = 0 0 0 1\\nrotate = 90\\n/'), "frame 2 saved");
  await key("Enter");
  await until("sel === 3", "frame 3");
  // frame 3: a crop (its right edge dragged in), kept as the job's window
  const before = await ev('fetch(S("strip.preview.job")).then(r=>r.text())');
  await dragRight(40);
  await until('fetch(S("strip.preview.job")).then(r=>r.text()).then(t=>t!==' + JSON.stringify(before) + ')', "crop kept");
  const after = await ev('fetch(S("strip.preview.job")).then(r=>r.text())');
  await key("Enter");
  await until("sel === 4", "frame 4");
  // frame 6 (partial) is under construction: Enter refused
  await ev("pick(6)");
  await until("sel === 6", "frame 6");
  const uc = await ev('document.getElementById("th6").querySelector(".b").textContent');
  await key("Enter");
  const ucSaid = await ev('document.getElementById("rst").textContent');
  // 4 and 5 skipped (x): none pending, the scan starts
  await ev("pick(4)");
  await until("sel === 4", "back to 4");
  await key("x");
  await until("sel === 5", "frame 5");
  await ev('window.stSeen=[];new MutationObserver(()=>stSeen.push(document.getElementById("rst").textContent)).observe(document.getElementById("rst"),{childList:true,characterData:true,subtree:true})');
  await key("x");
  await until('!document.getElementById("toast").hidden || (task && task.op === "scan")', "starting toast");
  await until('task && task.op === "scan" && task.state === "done" && landed.length > 0', "scan", 90000);
  await ev("landQ");
  const review = await ev('fetch(S("strip.review")).then(r=>r.text())');
  const job1 = await ev('fetch(S("strip.job")).then(r=>r.text())');
  const landed1 = await ev("landed.slice()");
  const states = await ev('frames.map(f=>stateOf(f))');
  const ppin = await ev('document.getElementById("ppin").textContent');
  // downloads: frame 1 and every landed frame
  await ev("pick(1)");
  await until('!document.getElementById("dl").disabled', "download ready");
  await ev('document.getElementById("dl").click()');
  await ev('document.getElementById("all").click()');
  const want = [strip + "_frame01.jpg", strip + "_frame01.jpg.txt", strip + ".zip"];
  for (let t = 0; t < 600 && !want.every(f => existsSync(join(dl, f))); t++) await sleep(100);
  // a crop on a scanned frame asks; R: a rescan of that frame alone
  await dragRight(20);
  await until('document.getElementById("dlg").open', "rescan dialog");
  const question = await ev('document.getElementById("dq").textContent');
  await cdp("Input.dispatchKeyEvent", { type: "keyDown", key: "r", text: "r" }, sid);
  await until('task && task.state === "running"', "rescan started", 20000);
  await until('task.state === "done"', "rescan done", 90000);
  await ev("landQ");
  const keymapText = await ev('(()=>{keymap();const t=document.getElementById("km").textContent;keymap();return t})()');
  await sleep(300);
  const out = await ev('(async()=>({landed: landed.slice(), seen: stSeen, plot: !document.getElementById("fz").hidden,' +
    ' notes: document.getElementById("notes").textContent, print: await fetch(S("strip.print")).then(r=>r.text()),' +
    ' files: (await api(S("files"))).map(f=>f.name), job2: await fetch(S("strip.job")).then(r=>r.text())}))()');
  console.log(JSON.stringify(Object.assign({ strip, res, holder, recent, scanFile, before, after, review,
    job1, landed1, states, ppin, uc, ucSaid, question, keymapText }, out)));
} catch (e) {
  console.error(String(e && e.stack || e));
  process.exitCode = 1;
} finally {
  try { ws && ws.close(); } catch (e) {}
  proc.kill();
  await sleep(200);
  rmSync(prof, { recursive: true, force: true });
}
