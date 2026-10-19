// SPDX-License-Identifier: GPL-2.0-or-later
// The page end to end in a headless Chromium (DevTools protocol over
// node's WebSocket; node >= 22): a user's choices, keys and drags against
// ftwebd + the fake ftpoc, the downloads saved to a folder.
// tests/test_page.py checks them.
//
//   node page_e2e.mjs <chromium> <http://127.0.0.1:port/> <download dir> [fail|batch] [hold file]
//
// Prints one JSON line: {strip, res, live, landed, ...}. fail: the scan's
// window does not come (a proxy in front of ftwebd drops it until the
// page asks GET /_proxy/open); then it lands as a later window would.
// batch: confirms while a job scans collect into the next job (scans wait
// while the hold file exists: fake_ftpoc.py).
import { spawn } from "node:child_process";
import { mkdtempSync, readFileSync, existsSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";

const [chrome, url, dl, mode, hold] = process.argv.slice(2);
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

// batch: frame 1 confirmed starts a job at once (held running), 2 then
// queues behind it, 3 merges into that queued job (PUT); then the hold
// goes and both land
async function batchFlow() {
  const out = {}, scans = '[...bj.values()].filter(j=>j.op==="scan")',
    jobsNow = scans + '.map(j=>[j.id,j.state,j.frames]).sort((a,b)=>a[0]-b[0])',
    badges = ns => ev("[" + ns + '].map(n=>document.getElementById("th"+n).querySelector(".b").textContent)');
  // after a 507 (full=1) the frames wait: a snapshot re-arms them only
  // when a job left the batch's queued|running set (none of this strip's
  // jobs exist yet; maybeStart counted, not run)
  out.rearm = await ev('(async()=>{const m=maybeStart;let c=0;maybeStart=()=>{c++};try{' +
    'const J=(id,s)=>({id,op:"scan",strip:"other",state:s,exit:0}),n=[];full=1;' +
    'for(const js of [[J(1,"running"),J(2,"queued")],[J(1,"running"),J(2,"queued")],' +
    '[J(1,"done"),J(2,"running")],[J(1,"done"),J(2,"running")]]){await onBatch({rev:0,jobs:js});n.push(c)}' +
    'return [n,full]}finally{maybeStart=m}})()');
  // jobs of this strip the page did not plan (synthetic snapshots): a job
  // file that cannot be fetched (a network error) is not kept, so the
  // next snapshot tries again; an ended job without a file (it never
  // started: the box removed it) leaves its confirmed frames to retry,
  // not sent again by themselves
  out.adopt = await ev('(async()=>{const g=get,J=(id,s)=>({id,op:"scan",strip:cur,state:s,exit:-1});try{' +
    'get=async p=>{if(/strip_j98\\.job$/.test(p))throw new TypeError("Failed to fetch");return g(p)};' +
    'await onBatch({rev:0,jobs:[J(98,"queued")]});const kept=bj.has(98);get=g;' +
    'await onBatch({rev:0,jobs:[J(98,"queued")]});const again=bj.has(98);' +
    'dec[3]="confirmed";await onBatch({rev:0,jobs:[J(99,"failed")]});' +
    'return [kept,again,stateOf(frame(3)),todoFrames()]}' +
    'finally{get=g;bj.delete(98);bj.delete(99);failed.clear();delete dec[3];live.clear();for(const f of frames)badge(f)}})()');
  writeFileSync(hold, "");
  await ev("startMs=200");
  await key("Enter");
  await until(scans + '.some(j=>j.state==="running")', "job 1 running", 20000);
  out.first = await ev(jobsNow);
  await until("sel === 2", "frame 2");
  await key("Enter");
  await until(scans + ".length === 2", "job 2 queued", 20000);
  await until("sel === 3", "frame 3");
  await key("Enter");
  await until(scans + '.some(j=>j.state==="queued"&&j.frames.join()==="2,3")', "3 merged", 20000);
  out.jobs = await ev(jobsNow);
  out.tailJob = await ev('fetch(F("strip_j' + out.jobs[1][0] + '.job")).then(r=>r.text())');
  out.badges = await badges([1, 2, 3]);
  // Backspace on a queued frame: its job re-planned without it (PUT)
  await ev("pick(3)");
  await until("sel === 3", "frame 3");
  await key("Backspace");
  await until(scans + '.some(j=>j.state==="queued"&&j.frames.join()==="2")', "3 out", 20000);
  out.tailAfter = await ev('fetch(F("strip_j' + out.jobs[1][0] + '.job")).then(r=>r.text())');
  out.badge3 = (await badges([3]))[0];
  // the job's last frame out: the job dropped (DELETE)
  await ev("pick(2)");
  await until("sel === 2", "frame 2");
  await key("Backspace");
  await until("!" + scans + '.some(j=>j.state==="queued")', "job dropped", 20000);
  out.dropped = await ev('api("/api/tasks/' + out.jobs[1][0] + '")');
  // a frame of the running job: refused, Stop offered (Skip disabled)
  await ev("pick(1)");
  await until("sel === 1", "frame 1");
  out.skipOff = await ev('[document.getElementById("skip").disabled,document.getElementById("stop").disabled]');
  await key("Backspace");
  out.running = await ev('document.getElementById("rst").textContent');
  // frame 2 confirmed again: a job of its own
  await ev("pick(2)");
  await until("sel === 2", "frame 2");
  out.skipOn = await ev('document.getElementById("skip").disabled');
  await key("Enter");
  await until(scans + '.some(j=>j.state==="queued"&&j.frames.join()==="2")', "2 again", 20000);
  // the batch's changes wait (bq held by a gate): the decision the user
  // made last is the one that holds
  const gate = 'window.gate=new Promise(y=>window.openGate=y);bq=bq.then(()=>gate);0';
  // 3 confirmed, the countdown fires (its enqueue waits), then Backspace:
  // not sent
  await ev(gate);
  await ev("pick(3)");
  await until("sel === 3", "frame 3");
  await key("Enter");
  await until("!startT", "countdown fired", 20000);
  await ev("pick(3)");
  await until("sel === 3", "frame 3");
  await key("Backspace");
  await ev("openGate();bq");
  out.late = await ev("[!!jobOf(3),dec[3]]");
  // 2 out of its queued job (waits), then confirmed again: it stays in
  await ev(gate);
  await ev("pick(2)");
  await until("sel === 2", "frame 2");
  await key("Backspace");
  await key("Enter");
  await ev("openGate();bq");
  out.again = await ev("[jobOf(2)&&jobOf(2).frames,dec[2]]");
  // a reload: the batch rebuilt from the box (its snapshot + job files)
  await until(fileHas("strip.review", "/^3 pending$/m"), "review kept");
  out.before = await ev(jobsNow + '.filter(x=>x[1]!=="done")');
  await cdp("Page.reload", {}, sid);
  await until('typeof bj === "object" && document.querySelectorAll(".th").length === 6 && ' +
    scans + '.filter(j=>!j.closed).length === 2 && document.getElementById("th1").querySelector(".b").textContent === "scanning"', "rebuilt", 30000);
  out.after = await ev(jobsNow + '.filter(x=>x[1]!=="done")');
  out.badgesAfter = await badges([1, 2, 3]);
  out.bat = await ev('document.getElementById("bat").textContent');
  await ev("startMs=200");
  // Stop: the running job ends stopped, its frame to retry; the next runs
  await until('!document.getElementById("stop").disabled', "stop enabled");
  await ev('document.getElementById("stop").click()');
  await until('document.getElementById("th1").querySelector(".b").textContent === "retry"', "frame 1 to retry", 20000);
  out.retry = (await badges([1]))[0];
  // Confirm on it: a retry, a job of its own
  await ev("pick(1)");
  await until("sel === 1", "frame 1");
  await key("Enter");
  await until("jobOf(1) && jobOf(1).id === 5", "retried", 20000);
  rmSync(hold);
  await until(scans + ".every(j=>j.closed) && landed.includes(1) && landed.includes(2)", "batch landed", 90000);
  await ev("landQ");
  out.landed = await ev("landed.slice()");
  // Clear: the queued job dropped, its frame to retry
  writeFileSync(hold, "");
  await ev("pick(4)");
  await until("sel === 4", "frame 4");
  await key("Enter");
  await until('jobOf(4) && jobOf(4).state === "running"', "frame 4 running", 20000);
  await ev("pick(5)");
  await until("sel === 5", "frame 5");
  await key("Enter");
  await until('jobOf(5) && jobOf(5).state === "queued" && !document.getElementById("clr").hidden', "frame 5 queued", 20000);
  await ev('document.getElementById("clr").click()');
  await until('document.getElementById("th5").querySelector(".b").textContent === "retry"', "cleared", 20000);
  out.cleared = await badges([4, 5]);
  rmSync(hold);
  await until('landed.includes(4) && document.getElementById("bat").textContent === "" && document.getElementById("clr").hidden', "frame 4 landed", 90000);
  await ev("landQ");
  out.landed2 = await ev("landed.slice()");
  out.files = await ev('api(S("files")).then(l=>l.map(f=>f.name))');
  return out;
}

run: try {
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
  if (mode === "batch") {
    console.log(JSON.stringify(Object.assign({ strip }, await batchFlow())));
    break run;
  }
  // the scan waits for the last decision (the countdown restarts on each)
  await ev("startMs=1e9");
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
  // the large view laid out for frame 3 (Enter's pick is not awaited: sel
  // turns 3 at once, G holds frame 2's view until its drawBig): the drag
  // is aimed through G
  await ev("pick(3)");
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
  // fail: frame 1 alone (the fake window holds frame 1 only; 2 and 3 not
  // landing would start a scan of their own)
  if (mode === "fail") for (const n of [2, 3]) {
    await ev("pick(" + n + ")");
    await until("sel === " + n, "frame " + n);
    await key("x");
    await until("sel === 5", "frame 5");
  }
  await ev('window.stSeen=[];new MutationObserver(()=>stSeen.push(document.getElementById("rst").textContent+" |band "+(fzAt&&fzAt[2]))).observe(document.getElementById("rst"),{childList:true,characterData:true,subtree:true})');
  await ev("startMs=3000");
  await key("x");
  await until('!document.getElementById("toast").hidden || (task && task.op === "scan")', "starting toast");
  if (mode === "fail") {
    // the window lost for good: said, nothing wedged, no rescan for it
    await until('task && task.op === "scan" && task.state === "done"', "scan", 90000);
    await ev("landQ");
    await until('document.getElementById("fbad").textContent', "the lost window said", 60000);
    await sleep(500);
    const bad = await ev('document.getElementById("fbad").textContent');
    const landedFail = await ev("landed.slice()");
    const restarted = await ev('!document.getElementById("toast").hidden || !!startT || task.state === "running"');
    const job1 = await ev('fetch(F("strip_j"+task.id+".job")).then(r=>r.text())');
    // a window landing after it (here the same one, now through) lands
    await ev('fetch("/_proxy/open")');
    await ev('land("strip_j"+task.id+"_win01.raw")');
    await ev("landQ");
    const landed = await ev("landed.slice()");
    const badAfter = await ev('document.getElementById("fbad").textContent');
    await ev("pick(1)");
    await until('!document.getElementById("dl").disabled', "download ready");
    await ev('document.getElementById("dl").click()');
    const f = join(dl, strip + "_frame01.jpg");
    for (let t = 0; t < 600 && !existsSync(f); t++) await sleep(100);
    await sleep(300);
    console.log(JSON.stringify({ strip, bad, landedFail, restarted, job1, landed, badAfter }));
  } else {
    await until('task && task.op === "scan" && task.state === "done" && landed.length > 0', "scan", 90000);
    await ev("landQ");
    const review = await ev('fetch(S("strip.review")).then(r=>r.text())');
    const job1 = await ev('fetch(F("strip_j"+task.id+".job")).then(r=>r.text())');
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
      ' files: (await api(S("files"))).map(f=>f.name), job2: await fetch(F("strip_j"+task.id+".job")).then(r=>r.text())}))()');
    console.log(JSON.stringify(Object.assign({ strip, res, holder, recent, scanFile, before, after, review,
      job1, landed1, states, ppin, uc, ucSaid, question, keymapText }, out)));
  }
} catch (e) {
  console.error(String(e && e.stack || e));
  process.exitCode = 1;
} finally {
  try { ws && ws.close(); } catch (e) {}
  proc.kill();
  await sleep(200);
  rmSync(prof, { recursive: true, force: true });
}
