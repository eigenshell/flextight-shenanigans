// SPDX-License-Identifier: GPL-2.0-or-later
// The review screen (index.html loads it deferred): the live preview bar
// and the strip's frames as the preview arrives (the engine's streaming
// detector seals each frame once the gap after it shows: it can be judged
// at once and is locked from then on), one at a time (a positive of each, its name,
// crop, turn and C/M/Y/D, confirm or skip), the job list (confirmed frames
// go to the box's batch a moment after the last decision; a crop on a
// frame already scanned asks for a rescan), the prints as frames land,
// the downloads and the keymaps (qwerty, dvorak). Shares index.html's globals ($, ft, api, get, cur,
// ...). Numbers and pixels come from the engine (ftweb.js); this is glue.
"use strict";
const CSS=`#rv .bar{background:linear-gradient(#262019,#120f0d);border-block:1px solid var(--line);padding:8px 0;margin-top:6px}
#live{display:block;width:100%;height:auto;max-height:140px;object-fit:contain;cursor:pointer;image-rendering:auto}
.prog{margin:8px 0 2px;height:6px;background:var(--panel);border:1px solid var(--line);overflow:hidden}.prog div{height:100%;width:0;background:var(--amber);transition:width .3s}
#rv .st{flex:1;font-size:13px;min-height:1.4em}#rv .st.err,#fbad{color:var(--red)}.stop{border-color:var(--red);color:var(--red)}
#fz{display:block;width:240px;height:64px}
#thumbs{display:flex;gap:8px;overflow-x:auto;padding:8px 0}
.th{position:relative;flex:none;border:2px solid transparent;background:#000;cursor:pointer;padding:0;min-height:0}
.th canvas{display:block;height:84px}.th.sel{border-color:var(--amber)}
.th span{position:absolute;left:3px;font-size:10px;line-height:1.3;padding:0 4px;background:#000b}.th .n{top:3px}.th .b{bottom:3px}
.b.confirmed,.b.queued{color:var(--amber)}.b.scanning{color:#ffd04a}.b.done{color:var(--ok)}.b.skipped,.b.under{color:var(--dim)}.b.retry{color:var(--red)}
.view{display:grid;grid-template-columns:1fr 260px;gap:14px;align-items:start}
@media (max-width:760px){.view{grid-template-columns:1fr}}
#bw{background:#000;text-align:center;overflow:hidden}#bw.z{overflow:auto;max-height:80vh;text-align:left}
#big{max-width:100%;vertical-align:top;touch-action:none}#bw.z #big{max-width:none}
aside input{width:100%}aside .row{margin-top:8px}
.k{display:flex;align-items:center;gap:6px;margin-top:6px}.k i{width:1.2em;font-style:normal;color:var(--dim)}.k b{width:2.6em;text-align:center}.k button{min-width:40px;padding:6px 0}
#fno{color:var(--amber)}#jobs td:last-child{color:var(--dim)}
pre{white-space:pre-wrap;word-break:break-all;max-height:240px;overflow:auto;background:#0008;padding:8px;font-size:12px}
details{color:var(--dim);font-size:12px;margin-top:12px}
#toast{position:fixed;left:50%;bottom:20px;transform:translateX(-50%);background:var(--panel);border:1px solid var(--amber);padding:10px 14px;display:flex;gap:12px;align-items:center;z-index:5}
dialog{background:var(--panel);color:var(--ink);border:1px solid var(--amber);max-width:420px}
#km{position:fixed;inset:10% 10% auto;background:var(--panel);border:1px solid var(--line);padding:14px;z-index:6;max-height:80vh;overflow:auto}
#km td{padding:3px 10px 3px 0;border:0}kbd{color:var(--amber);font:inherit}`;
const HTML=`<div class="bar"><canvas id="live" width="1" height="1" aria-label="the strip"></canvas></div>
<div class="prog"><div id="pb"></div></div>
<div class="row"><span class="st" id="rst"></span><span class="note" id="bat"></span><button id="clr" hidden>Clear queue</button><button id="again" hidden>Preview again</button><button id="stop" class="stop" disabled>Stop</button></div>
<div class="note"><span id="fet"></span> <span id="fbad"></span> <span id="gridn"></span></div>
<canvas id="fz" hidden></canvas>
<div id="thumbs"></div>
<div class="view"><div id="bw"><canvas id="big" width="0" height="0"></canvas></div>
<aside><div>frame <b id="fno">—</b> · <span id="fst" class="b"></span></div>
<div class="row"><input id="name" aria-label="Frame name" spellcheck="false" autocomplete="off"></div>
<div id="keys"></div>
<div class="row"><button id="tl" title="turn left">↺</button><button id="tr" title="turn right">↻</button><button id="zm" title="zoom 1:1 / fit">1:1</button></div>
<div class="row"><button id="prev" aria-label="previous frame">‹</button><button id="skip">Skip</button><button id="ok" class="go">Confirm</button><button id="next" aria-label="next frame">›</button></div>
<div class="row"><button id="dl">Download</button><button id="all">All (zip)</button></div>
<p class="note"><a href="#" id="kh">keys (?)</a></p></aside></div>
<h2>Jobs</h2><table id="jobs"></table><div id="ppin" class="note"></div><div id="notes" class="note"></div>
<details><summary>Log</summary><pre id="log"></pre></details>
<div id="toast" hidden><span id="tt"></span><button id="tc">Cancel</button></div>
<dialog id="dlg"><p id="dq"></p><div class="row"><button id="dr">Rescan (R)</button><button id="dk">Keep old crop (K)</button><button id="dx">Cancel (Esc)</button></div></dialog>
<div id="km" hidden></div>`;

// ---------------------------------------------------------------- state
let frames=[],dec={},landed=[],sel=0,task=null,run={kind:"",n:1,i:1},scanPx=0,info={},meta="";
let pvOk=0,pvImg=null,kk={},scanning=new Set(),rescan=new Set(),cropped=new Set(),groups={},dofs={},zoomed=0;
// this strip's jobs in the box's batch, by id: {id, op, state, frames,
// groups, dofs, rescan, watched, exited, closed}; frames whose job ended
// without them (to retry: never sent again by themselves)
let bj=new Map(),failed=new Set(),bes=null;
// windows cut here (a replayed job announces them again)
const cutDone=new Set();
// frames of a window scanned but not fetched (no rescan for them; a
// reload fetches again), and why, per window
let unfetched=new Set(),fe={};
// the streamed preview: raw lines not yet fed (before strip.raw.cal is
// there), begun, the holder grid used; waiting: every frame decided, the
// next sealed frame is picked; jobPut: strip.job is the page's
let sv={begun:0,pend:[],at:0,grid:0,dead:0},waiting=0,jobPut=0,jobDirty=0;
const NN=n=>String(n).padStart(2,"0"),frame=n=>frames.find(f=>f.n===n);
const autoName=n=>cur+"_frame"+NN(n);
// partial and half frames: under construction. A window keeps one pixel
// count, so their shorter long side would raise the ppi of their whole
// run; they wait for per-frame resolution
const UC="under construction",blocked=f=>f.type==="partial"||f.type==="half";
function stateOf(f){return blocked(f)?UC:scanning.has(f.n)?"scanning":landed.includes(f.n)&&!rescan.has(f.n)?"done":jobOf(f.n)?"queued":failed.has(f.n)?"retry":dec[f.n]||"pending"}
const started=f=>/queued|scanning|done/.test(stateOf(f));

// ---------------------------------------------------------------- screen
async function review(s){
 if(!$("live")){const st_=document.createElement("style");st_.textContent=CSS;document.head.append(st_);$("rv").innerHTML=HTML;wire()}
 frames=[];dec={};landed=[];sel=0;kk={};scanning.clear();bj.clear();failed.clear();cutDone.clear();for(const e of ess.values())e.close();ess.clear();task=null;rescan.clear();cropped.clear();groups={};dofs={};pvOk=0;pvImg=null;unfetched.clear();fe={};fetchBad();fetching("");
 sv={begun:0,pend:[],at:0,grid:0,dead:0};waiting=0;jobPut=0;jobDirty=0;gridNote();
 $("thumbs").innerHTML="";$("jobs").innerHTML="";$("notes").textContent="";$("ppin").textContent="";$("log").textContent="";$("pb").style.width="0";
 $("again").hidden=true;drawBig();
 const t=await api("/api/tasks/current").catch(()=>null);
 info=parseKV(await get(S("files/strip.scan"))||"");
 scanPx=0;if(info.ppi)try{scanPx=(await ft("resolutions",[+info.ppi]))[0].px}catch(e){st(e.message,1)}
 // the batch followed once the running preview's stream is reset (its
 // replayed lines feed the new one)
 if(t&&t.state==="running"&&t.strip===s){watch(t);if(t.op==="preview"){liveReset();await streamStart();batchWatch();return}}
 batchWatch();await load()}
function parseKV(t){const o={};for(const l of t.split("\n")){const m=l.match(/^(\w+) = (.*)$/);if(m)o[m[1]]=m[2]}return o}
const emOf=()=>info.emulsion||(ems.find(e=>e.mode==="negative")||ems[0]).id;

// the standard preview's .txt before strip.raw.txt is written (after the raw)
const PVM=()=>"px="+(lv.px||666)+"\nx=2\nbin=12\n",slideOf=()=>filmOf(emOf())==="slide";
// strip.print and strip.review: the edits re-attach to frames by number
async function edits(){const s=cur,p=await get(S("strip.print"));if(s!==cur)return;
 try{if(p)await ft("printLoad",p);else await ft("printNew",emOf())}catch(e){st("strip.print: "+e.message,1);await ft("printNew",emOf())}
 dec={};for(const l of(await get(S("strip.review"))||"").split("\n")){const m=l.match(/^(\d+) (\w+)$/);if(m)dec[+m[1]]=m[2]}}
// the strip's files -> frames (the preview replayed through the streaming
// detector: the same frames it showed), decisions, prints
async function load(){const s=cur;
 const[raw,cal,faul,mt]=await Promise.all([get(F("strip.raw"),1),get(F("strip.raw.cal"),1),get(F("strip.raw.faul"),1),get(F("strip.raw.txt"))]);if(s!==cur)return;
 if(!raw||!cal){$("again").hidden=!!(task&&task.state==="running");if(!$("rst").classList.contains("err"))st("no preview yet");return}
 meta=mt||PVM();await edits();if(s!==cur)return;
 const row=+((meta.match(/^px=(\d+)/m)||[0,666])[1])*6;
 try{await ft("streamBegin",cal,meta,faul,slideOf());const a=await ft("streamFeed",raw.subarray(0,raw.length-raw.length%row)),b=await ft("streamEnd");sv.grid=a.grid||b.grid;pvOk=1}
 catch(e){return st("preview: "+e.message,1)}
 frames=(await ft("jobJson",meta)).frames;await ownJob();if(s!==cur)return;
 frames=(await ft("jobJson",meta)).frames;gridNote();noFile();
 pvImg=await img(F("strip.png")+"?t="+Date.now());
 // the frames from the preview at once; scanned windows (tens of MB each)
 // are cut after, each frame redrawn as its print lands
 drawStrip();jobs();const first=frames.find(f=>stateOf(f)==="pending")||frames[0];
 await thumbs();if(first)await pick(first.n);st("");
 const l=await api(S("files")).catch(()=>[]),wins=winFiles(l.map(x=>x.name));
 for(const[i,n]of wins.entries())landQ=landQ.then(async()=>{if(s!==cur)return;st("loading scanned window "+(i+1)+"/"+wins.length+" …");
  const before=landed.slice();await cut(n,1);const nw=landed.filter(x=>!before.includes(x));if(s!==cur)return;
  await reprint();await thumbs(landed.slice());jobs();drawStrip();if(landed.includes(sel))await pick(sel);if(i===wins.length-1)st("")}).catch(e=>st(e.message,1))}
// strip.job: the page's (it differs from the box's strip.preview.job):
// its windows (crops) onto the replayed frames; else the page writes it
async function ownJob(){const[j,pj]=await Promise.all([get(S("strip.job")),get(F("strip.preview.job"))]);
 if(j&&j!==pj)try{await ft("streamWindows",j);await ft("planLoad",j);jobPut=1;return}catch(e){st("strip.job: "+e.message,1)}
 await putJob()}
// the page's strip.job: its frames planned (ft_share_plan_sizes: skipped
// and under construction 0); not while a preview runs (ftpoc frames writes
// it then; a scan job has its own strip_j<id>.job)
async function putJob(){if(!frames.length)return;if(task&&task.state==="running"&&task.op==="preview"){jobDirty=1;return}
 const sz=frames.map(f=>blocked(f)||dec[f.n]==="skipped"?0:scanPx);if(!sz.some(x=>x>0))return;
 try{const r=await ft("plan",sz);await api(S("strip.job"),{method:"PUT",body:r.text});jobPut=1;jobDirty=0}catch(e){st("strip.job: "+e.message,1)}}
const gridNote=()=>$("gridn").textContent=sv.grid?"frames from the holder grid (no gap seen between some frames): check their boxes":"";
// the streamed preview: a new strip's frames from nothing
async function streamStart(){sv={begun:0,pend:[],at:0,grid:0,dead:0};frames=[];sel=0;waiting=0;pvOk=0;$("thumbs").innerHTML="";meta=PVM();
 await edits();jobs();gridNote();drawBig()}
// raw lines (whole) to the stream: begun once strip.raw.cal is there (it is
// written before the raw); till then they wait and the live bar goes on
async function streamGrow(b,now){if(sv.dead)return;if(b&&b.length)sv.pend.push(b);
 try{if(!sv.begun){if(!now&&Date.now()-sv.at<1000)return;sv.at=Date.now();
   const[cal,faul]=await Promise.all([get(F("strip.raw.cal"),1),get(F("strip.raw.faul"),1)]);if(!cal)return;
   try{await ft("streamBegin",cal,PVM(),faul,slideOf())}catch(e){return}sv.begun=1;pvOk=1}
  while(sv.pend.length)await sealed(await ft("streamFeed",sv.pend.shift()))}
 catch(e){sv.dead=1;st("preview frames: "+e.message,1)}}
// frames the detector sealed: to the list, judged at once, locked
async function sealed(r){if(!r)return;if(r.grid&&!sv.grid){sv.grid=1;gridNote()}if(!r.frames.length)return;
 for(const f of r.frames){frames.push(f);addThumb(f)}jobs();
 if(!sel||waiting){const p=r.frames.find(f=>stateOf(f)==="pending")||r.frames[0];await pick(p.n)}
 await thumbs(r.frames.map(f=>f.n))}
// the preview ended (ok) or stopped: the rest of its lines, the open run
// sealed (ok) or dropped (stopped); strip.job if a frame was sealed
async function previewDone(ok){const s=cur;
 if(!sv.begun||lv.bad||sv.dead)return load();
 await liveGrow(0,1);await streamGrow(null,1);if(s!==cur)return;
 if(ok)await sealed(await ft("streamEnd").catch(e=>(st("preview: "+e.message,1),null)));
 meta=await get(F("strip.raw.txt"))||meta;await putJob();
 pvImg=await img(F("strip.png")+"?t="+Date.now());drawStrip();jobs();
 if(!frames.length){$("again").hidden=false;st(ok?"no frame found":"stopped: no frame sealed")}}
const img=src=>new Promise(y=>{const i=new Image();i.onload=()=>y(i);i.onerror=()=>y(null);i.src=src});
// window scans: earlier runs first (old strips: strip_rK_winNN, then
// strip_winNN), then per job in id order (strip_j<id>_winNN)
function winFiles(names){const k=n=>{const m=n.match(/^strip_(?:r(\d+)_|j(\d+)_)?win(\d\d)\.raw$/);return m?[m[1]?0:m[2]?2:1,m[1]?+m[1]:m[2]?+m[2]:0,+m[3]]:null};
 return names.filter(k).sort((a,b)=>{const x=k(a),y=k(b);return x[0]-y[0]||x[1]-y[1]||x[2]-y[2]})}
// a scanned window: its raw streamed (fetchBig), then cut. Never throws:
// a window not fetched is said (#fbad) and its frames wait for a reload
async function cut(name,quiet){const s=cur;let fr=[],mb="";if(cutDone.has(name))return;try{
 const[cal,mt,faul]=await Promise.all([get(F(name+".cal"),1),get(F(name+".txt"),1),get(F(name+".faul"),1)]);
 if(s!==cur)return;if(!cal||!mt)return quiet||st(name+": files missing",1);
 fr=(new TextDecoder().decode(mt).match(/^share_frame=\d+/gm)||[]).map(x=>+x.slice(12));
 const raw=await fetchBig(F(name),(k,n)=>{if(s!==cur)return 0;const m=MB(k)+"/"+MB(n);if(m!==mb){mb=m;fetching(name+": fetching "+m+" MB")}return 1});
 if(s!==cur)return;if(!raw)return quiet||st(name+": files missing",1);
 for(const x of fr)unfetched.delete(x);delete fe[name];fetchBad();
 try{const r=await ft("cut",raw,cal,mt,faul);for(const f of r.frames){if(!landed.includes(f.frame))landed.push(f.frame);rescan.delete(f.frame);failed.delete(f.frame)}landed.sort((a,b)=>a-b);cutDone.add(name)}
 catch(e){if(!quiet)st(name+": "+e.message,1)}}
 catch(e){if(s!==cur)return;for(const x of fr)unfetched.add(x);fe[name]=name+": "+e.message;fetchBad()}
 finally{if(s===cur)fetching("")}}
// a window's raw: hundreds of MB, most of an hour over the box's Wi-Fi.
// Streamed with its progress (on(bytes, of n); 0 from on stops: null); a
// broken transfer goes on by Range from the bytes in hand (ftwebd answers
// 1 MB a time), after a pause while attempts in a row bring nothing;
// null if the file is missing
const MB=b=>(b/1048576).toFixed(0),fetching=m=>$("fet").textContent=m,fetchBad=()=>$("fbad").textContent=Object.values(fe).join(" · ");
async function fetchBig(p,on){let buf=null,k=0,n=0;
 for(let fails=0;;){const k0=k;let e="";
  try{const r=await fetch(p,k?{headers:{Range:"bytes="+k+"-"}}:{});if(r.status===404&&!buf)return null;if(!r.ok)throw new Error(r.status+" "+r.statusText);
   if(!buf){n=+r.headers.get("Content-Length");buf=new Uint8Array(n)}
   for(const rd=r.body.getReader();;){const{done,value}=await rd.read();if(done)break;if(k+value.length>n)throw new Error("longer than "+n+" bytes");
    buf.set(value,k);k+=value.length;if(!on(k,n)){rd.cancel();return null}}
   if(k>=n)return buf}
  catch(x){e=x.message}
  fails=k>k0?0:fails+1;if(fails>4)throw new Error("stopped at "+MB(k)+(n?"/"+MB(n):"")+" MB: "+(e||"no data"));
  if(e)await new Promise(y=>setTimeout(y,500*(fails+1)))}}
async function reprint(){if(!landed.length)return;try{$("notes").textContent=await ft("measure")}catch(e){st(e.message,1)}}

// the strip bar: the preview as it comes in, then strip.png with the boxes
let lv={off:0,first:1,rest:new Uint8Array(0),n:0,px:0,line:0,busy:0,at:0,bad:0};
function liveReset(){const c=$("live");lv={off:0,first:1,rest:new Uint8Array(0),n:0,px:0,line:0,busy:0,at:0,bad:0};c.width=1;c.height=1}
async function liveGrow(n,force){if(lv.bad||(!force&&(lv.busy||Date.now()-lv.at<900)))return;
 while(lv.busy)await new Promise(y=>setTimeout(y,50));lv.busy=1;lv.at=Date.now();
 try{if(!lv.px)lv.px=await ft("previewPx");const c=$("live");if(n&&c.width!==n){c.width=n;c.height=lv.px;lv.n=n}
  // ftwebd answers 1 MB a read: a few in a row to catch up (a reload)
  for(let i=0;i<(force?1e4:8);i++){
   const r=await fetch(F("strip.raw"),{headers:{Range:"bytes="+lv.off+"-"}});if(r.status===416)break;
   if(r.status!==206)throw new Error(r.status);const b=new Uint8Array(await r.arrayBuffer());if(!b.length)break;lv.off+=b.length;
   const all=new Uint8Array(lv.rest.length+b.length);all.set(lv.rest);all.set(b,lv.rest.length);
   const L=lv.px*6,k=Math.floor(all.length/L);lv.rest=all.slice(k*L);
   if(k){const chunk=all.slice(0,k*L),d=await ft("liveRows",chunk,lv.px,!slideOf(),lv.first);lv.first=0;
    const o=new OffscreenCanvas(d.w,d.h);o.getContext("2d").putImageData(new ImageData(new Uint8ClampedArray(d.rgba.buffer),d.w,d.h),0,0);
    if(c.width>1){const x=c.getContext("2d");x.setTransform(0,1,1,0,0,0);x.drawImage(o,0,lv.line);x.setTransform(1,0,0,1,0,0)}lv.line+=d.h;
    await streamGrow(chunk)}
   if(b.length<1<<20)break}}
 catch(e){lv.bad=1}finally{lv.busy=0}}
function drawStrip(){const c=$("live"),i=pvImg;if(!i)return;c.width=i.naturalWidth;c.height=i.naturalHeight;const x=c.getContext("2d");x.drawImage(i,0,0);
 for(const f of frames){const s=stateOf(f);x.lineWidth=f.n===sel?8:4;x.strokeStyle=f.n===sel?"#ff9a1f":s==="done"?"#7fd17f":s==="skipped"||blocked(f)?"#5a4e44":"#fff8";x.strokeRect(f.x,f.y,f.w,f.h)}}

// thumbnails: each frame as it will print (scanned: the scan's print)
function addThumb(f){const b=document.createElement("button");b.className="th";b.id="th"+f.n;
 b.innerHTML='<canvas></canvas><span class="n">'+f.n+'</span><span class="b"></span>';b.onclick=()=>pick(f.n);$("thumbs").append(b);badge(f)}
async function thumbs(only){const t=$("thumbs");if(!only){t.innerHTML="";for(const f of frames)addThumb(f)}   // every state at once, the prints after
 for(const f of frames)if(!only||only.includes(f.n)){badge(f);await paintTo($("th"+f.n).querySelector("canvas"),f.n,240).catch(()=>{})}}
function badge(f){if(f.n===sel){const r=inRun(f);$("skip").disabled=r;$("skip").title=r?RUN(f):""}
 const b=$("th"+f.n);if(!b)return;const s=stateOf(f);b.classList.toggle("sel",f.n===sel);const e=b.querySelector(".b");e.className="b "+s;e.textContent=s}
async function paintTo(c,n,edge){const d=landed.includes(n)?await ft("display",n,edge):pvOk?await ft("previewPrint",n,edge):null;if(!d)return null;
 c.width=d.w;c.height=d.h;c.getContext("2d").putImageData(new ImageData(new Uint8ClampedArray(d.rgba.buffer),d.w,d.h),0,0);return c}

// the large view: the print in its box, the strip around it, the crop
let G=null,drag=null,bigPrint=null;
async function drawBig(){const c=$("big"),f=frame(sel);if(!f){c.width=c.height=0;return}
 const bw=$("bw"),dpr=devicePixelRatio,r=await ft("rotation",f.n).catch(()=>0);
 if(zoomed){bigPrint=await paintTo(document.createElement("canvas"),f.n,2e4);if(!bigPrint)return;c.width=bigPrint.width;c.height=bigPrint.height;c.style.width=c.width/dpr+"px";c.getContext("2d").drawImage(bigPrint,0,0);G=null;return}
 c.style.width="";const b=f,m=0.12*Math.max(b.w,b.h),I=pvImg||(lv.line&&$("live").width>1?$("live"):null),iw=I?I.naturalWidth||I.width:b.x+b.w+m,ih=I?I.naturalHeight||I.height:b.y+b.h+m;
 const V={x:Math.max(0,b.x-m),y:Math.max(0,b.y-m)};V.w=Math.min(iw,b.x+b.w+m)-V.x;V.h=Math.min(ih,b.y+b.h+m)-V.y;
 const tw=r%180?V.h:V.w,th=r%180?V.w:V.h,CW=Math.round(bw.clientWidth*dpr),CH=Math.round(Math.min(innerHeight*.66,640)*dpr),k=Math.min(CW/tw,CH/th);
 c.width=Math.round(tw*k);c.height=Math.round(th*k);G={k,r,cx:c.width/2,cy:c.height/2,vx:V.x+V.w/2,vy:V.y+V.h/2,dpr};
 const x=c.getContext("2d");x.fillStyle="#000";x.fillRect(0,0,c.width,c.height);
 x.setTransform(1,0,0,1,0,0);x.translate(G.cx,G.cy);x.rotate(r*Math.PI/180);x.scale(k,k);x.translate(-G.vx,-G.vy);
 if(I){x.globalAlpha=.4;x.drawImage(I,V.x,V.y,V.w,V.h,V.x,V.y,V.w,V.h);x.globalAlpha=1}x.setTransform(1,0,0,1,0,0);
 // the thumbnail stands in at once; the print at full size replaces it
 const S=toScreen(drag?drag.box:f),tc=$("th"+f.n)&&$("th"+f.n).querySelector("canvas");
 if(!bigPrint&&tc&&tc.width>1)x.drawImage(tc,S.x,S.y,S.w,S.h);
 if(!bigPrint){const n=f.n;bigPrint=await paintTo(document.createElement("canvas"),n,Math.round(Math.max(S.w,S.h)));if(sel!==n)return}
 if(bigPrint)x.drawImage(bigPrint,S.x,S.y,S.w,S.h);
 x.strokeStyle=started(f)&&!rescan.has(f.n)?"#7fd17f":"#ff9a1f";x.lineWidth=2*dpr;x.strokeRect(S.x,S.y,S.w,S.h);x.fillStyle=x.strokeStyle;
 const h=7*dpr;for(const[px,py]of[[0,0],[.5,0],[1,0],[0,.5],[1,.5],[0,1],[.5,1],[1,1]])x.fillRect(S.x+px*S.w-h/2,S.y+py*S.h-h/2,h,h)}
// strip px <-> canvas px through G (rotation r about the view's centre)
function rot(dx,dy,deg){const a=deg*Math.PI/180,c=Math.round(Math.cos(a)),s=Math.round(Math.sin(a));return[dx*c-dy*s,dx*s+dy*c]}
function toScreen(b){const p=[[b.x,b.y],[b.x+b.w,b.y+b.h]].map(([x,y])=>{const[u,v]=rot(x-G.vx,y-G.vy,G.r);return[G.cx+u*G.k,G.cy+v*G.k]});
 return{x:Math.min(p[0][0],p[1][0]),y:Math.min(p[0][1],p[1][1]),w:Math.abs(p[1][0]-p[0][0]),h:Math.abs(p[1][1]-p[0][1])}}
function toStrip(S){const p=[[S.x,S.y],[S.x+S.w,S.y+S.h]].map(([x,y])=>{const[u,v]=rot((x-G.cx)/G.k,(y-G.cy)/G.k,-G.r);return[G.vx+u,G.vy+v]});
 return{x:Math.min(p[0][0],p[1][0]),y:Math.min(p[0][1],p[1][1]),w:Math.abs(p[1][0]-p[0][0]),h:Math.abs(p[1][1]-p[0][1])}}
function cpos(e){const c=$("big"),r=c.getBoundingClientRect();return[(e.clientX-r.left)*c.width/r.width,(e.clientY-r.top)*c.height/r.height]}
function onDown(e){const f=frame(sel);if(!G||!f)return;const[x,y]=cpos(e),S=toScreen(f),t=10*G.dpr;
 const L=Math.abs(x-S.x)<t,R=Math.abs(x-S.x-S.w)<t,T=Math.abs(y-S.y)<t,B=Math.abs(y-S.y-S.h)<t,inside=x>S.x&&x<S.x+S.w&&y>S.y&&y<S.y+S.h;
 if(!(L||R||T||B||inside))return;drag={L,R,T,B,move:!(L||R||T||B),x0:x,y0:y,S0:S,box:{x:f.x,y:f.y,w:f.w,h:f.h}};$("big").setPointerCapture(e.pointerId)}
function onMove(e){if(!drag)return;const[x,y]=cpos(e),dx=x-drag.x0,dy=y-drag.y0,S=Object.assign({},drag.S0),mn=24*G.dpr;
 if(drag.move){S.x+=dx;S.y+=dy}else{if(drag.L){S.x+=Math.min(dx,S.w-mn);S.w-=Math.min(dx,S.w-mn)}if(drag.R)S.w=Math.max(mn,S.w+dx);
  if(drag.T){S.y+=Math.min(dy,S.h-mn);S.h-=Math.min(dy,S.h-mn)}if(drag.B)S.h=Math.max(mn,S.h+dy)}
 drag.box=toStrip(S);drag.moved=1;drawBig()}
async function onUp(){if(!drag)return;const d=drag,f=frame(sel);drag=null;if(!d.moved||!f)return drawBig();await crop(f,d.box)}
// a crop: the job's window, kept (strip.job); a frame already in a scan
// asks first
async function crop(f,b){
 if(blocked(f)){st("frame "+f.n+" ("+f.type+"): "+UC,1);return drawBig()}
 if(started(f)&&!rescan.has(f.n)){const a=await ask("Frame "+f.n+" is already "+(stateOf(f)==="done"?"scanned":stateOf(f))+". A new crop needs a rescan.");
  if(a!=="rescan")return drawBig();rescan.add(f.n);dec[f.n]="confirmed";saveReview()}
 // the engine cuts the frame again; strip.job keeps the window (at the
 // preview's end while it runs: the box writes strip.job then)
 try{await ft("frameBox",f.n,[b.x,b.y,b.w,b.h],meta);
  Object.assign(f,{x:b.x,y:b.y,w:b.w,h:b.h});cropped.add(f.n);if(!(task&&task.state==="running"&&task.op==="preview"))await putJob();
  bigPrint=null;await drawBig();drawStrip();thumbs([f.n]);jobs();maybeStart()}
 catch(e){st(e.message,1);drawBig()}}
const ask=q=>new Promise(y=>{const d=$("dlg");$("dq").textContent=q;d.showModal();const done=v=>{d.close();d.onkeydown=null;y(v)};
 $("dr").onclick=()=>done("rescan");$("dk").onclick=()=>done("keep");$("dx").onclick=()=>done("cancel");d.oncancel=e=>{e.preventDefault();done("cancel")};
 d.onkeydown=e=>{const k=e.key.toLowerCase();if(k==="r")done("rescan");else if(k==="k")done("keep")}});

// one frame selected: its controls
async function pick(n){const f=frame(n);if(!f)return;sel=n;waiting=0;bigPrint=null;for(const g of frames)badge(g);
 $("fno").textContent=n;$("fst").className="b "+stateOf(f);$("fst").textContent=stateOf(f);
 $("name").placeholder=autoName(n);$("name").value=await ft("name",n).catch(()=>"");
 kk[n]=await ft("printGet",n).catch(()=>[0,0,0,0]);keys(kk[n]);$("dl").disabled=!landed.includes(n);$("all").disabled=!landed.length;
 drawStrip();await drawBig();const t=$("th"+n);if(t)t.scrollIntoView({block:"nearest",inline:"nearest"})}
function keys(k){$("keys").innerHTML="";"CMYD".split("").forEach((c,i)=>{const s=document.createElement("div");s.className="k";
 s.innerHTML="<i>"+c+'</i><button aria-label="'+c+' minus">−</button><b>'+(k[i]>0?"+":"")+k[i]+'</b><button aria-label="'+c+' plus">+</button>';
 const b=s.querySelectorAll("button");b[0].onclick=()=>click(i,-1);b[1].onclick=()=>click(i,1);$("keys").append(s)})}
let q=Promise.resolve();
const serial=fn=>q=q.then(fn).catch(e=>st(e.message,1));
const click=(i,d)=>serial(async()=>{const n=sel;if(!n)return;const k=kk[n].slice();k[i]+=d;await ft("clicks",n,k);kk[n]=k;if(n===sel){keys(k);bigPrint=null;await drawBig()}savePrint();thumbs([n])});
const reset=()=>serial(async()=>{const n=sel;if(!n)return;await ft("clicks",n,[0,0,0,0]);kk[n]=[0,0,0,0];keys(kk[n]);bigPrint=null;await drawBig();savePrint();thumbs([n])});
const turn=d=>serial(async()=>{const n=sel;if(!n)return;await ft("rotate",n,((await ft("rotation",n))+d+360)%360);bigPrint=null;await drawBig();savePrint();thumbs([n])});
let putT=0;
function savePrint(){clearTimeout(putT);putT=setTimeout(async()=>{try{await api(S("strip.print"),{method:"PUT",body:await ft("printText")})}catch(e){st(e.message,1)}},300)}
let revT=0;
function saveReview(){clearTimeout(revT);revT=setTimeout(async()=>{try{await api(S("strip.review"),{method:"PUT",body:frames.map(f=>f.n+" "+(blocked(f)?"skipped":dec[f.n]||"pending")).join("\n")+"\n"})}catch(e){st(e.message,1)}},200)}
async function setName(){const n=sel,v=$("name").value.trim();try{await ft("setName",n,v===autoName(n)?"":v);savePrint();st("")}catch(e){st(e.message,1)}}
function decide(d){const f=frame(sel);if(!f)return;
 if(blocked(f))return st("frame "+f.n+" ("+f.type+"): "+UC+": "+f.type+" frames wait for per-frame resolution",1);
 const j=jobOf(f.n);want[f.n]=d;
 if(j&&d!=="confirmed"&&stateOf(f)!=="done"){if(j.state!=="queued")return st(RUN(f),1);return unconfirm(f,j,d)}
 if(started(f)&&d==="pending")return st("frame "+f.n+" is "+stateOf(f)+": its decision stays",1);
 if(d==="confirmed")failed.delete(f.n);   // a retry: sent again
 dec[f.n]=d;saveReview();badge(f);jobs();drawStrip();
 if(d!=="pending"){const i=frames.indexOf(f),nx=frames.slice(i+1).concat(frames.slice(0,i)).find(g=>stateOf(g)==="pending");if(nx)pick(nx.n);else{pick(f.n);waiting=1}}
 maybeStart()}
// a frame out of its queued job: the job re-planned without it (PUT), or
// dropped when it was its last frame (DELETE: a job that started
// meanwhile is stopped, the frame out of it). PUT 409 (started), DELETE
// 409 (ended): it stays in, said. A decision made after it (want: the
// last one per frame) wins: then nothing changes here
const want={};
const unconfirm=(f,j,d)=>bq=bq.then(async()=>{if(want[f.n]!==d)return maybeStart();
 const stays=w=>{for(const g of frames)badge(g);jobs();st("frame "+f.n+": its job has just "+w+"; it stays in",1)};
 if(j.frames.includes(f.n)){const ns=j.frames.filter(n=>n!==f.n);
  if(ns.length){if(!await put(j,ns))return stays("started")}
  else{const r=await fetch("/api/tasks/"+j.id,{method:"DELETE"}),o=await r.json().catch(()=>({}));
   if(r.status===409)return stays("ended");
   if(!r.ok&&r.status!==404)throw new Error(o.error||r.status+" "+r.statusText);
   if(o.was==="running")j.frames=[];else bj.delete(j.id)}}
 dec[f.n]=d;saveReview();badge(f);jobs();drawStrip()}).catch(e=>st(e.message,1));
// a frame of the job that runs (not yet landed): no decision change, its
// job's Stop instead
const inRun=f=>{const j=jobOf(f.n);return!!j&&j.state!=="queued"&&stateOf(f)!=="done"},
 RUN=f=>"frame "+f.n+" is in the job scanning now: Stop ends that job (its frames to retry)";
function step(d){const i=frames.findIndex(f=>f.n===sel)+d;if(i>=0&&i<frames.length)pick(frames[i].n)}

// the job list; confirmed frames go to the box's batch (a FIFO of jobs
// run one after the other) a moment after the last decision
function jobs(){const t=$("jobs");t.innerHTML="<tr><th>Frame</th><th>Name</th><th>State</th></tr>";
 for(const f of frames){const r=t.insertRow();r.insertCell().textContent=f.n;r.insertCell().textContent=autoName(f.n);r.insertCell().textContent=stateOf(f);
  ft("name",f.n).then(v=>{if(v)r.cells[1].textContent=v}).catch(()=>{})}batInd()}
// the batch, said: this strip's frames still to come; Clear (shown with
// a queued job) drops the queued jobs, box-wide, their frames to retry
function batInd(){const left=new Set();let q=0;
 for(const j of bj.values())if(!j.closed&&j.op==="scan"){if(j.state==="queued")q=1;for(const n of j.frames)if(!landed.includes(n)||rescan.has(n))left.add(n)}
 $("bat").textContent=left.size?"Scanning · "+left.size+" frame"+(left.size>1?"s":"")+" left":"";$("clr").hidden=!q}
// through bq: no enqueue or unconfirm between the DELETE and the frames
// marked
const clearQ=()=>bq=bq.then(async()=>{await api("/api/tasks?state=queued",{method:"DELETE"});
 for(const[id,j]of bj)if(j.state==="queued"){for(const n of j.frames)failed.add(n);bj.delete(id)}
 jobs();for(const f of frames)badge(f);st("queue cleared: its frames to retry (Confirm)")}).catch(e=>st(e.message,1));
// a job of this strip the page did not plan (a reload, another tab):
// its frames from its job file on the box; an ended one's frames not
// done are to retry. Kept once its file is read: a fetch that fails
// leaves it to the next snapshot. An ended one without a file never
// started (the box removed it): this strip's confirmed frames not landed
// and in no open job are to retry, not sent again by themselves
async function adopt(x){if(bj.has(x.id))return;const j={id:x.id,op:x.op,state:x.state,frames:[],groups:{},dofs:{}};
 const left=[];if(x.op==="scan"){let t;try{t=await get(F("strip_j"+x.id+".job"))}catch(e){return}
  if(t==null&&ended(x.state))j.nofile=1;
  for(const c of(t||"").split(/^(?=\[frame \d+\])/m)){const m=c.match(/^\[frame (\d+)\]/);if(!m||!/^include = yes/m.test(c))continue;
   const n=+m[1],g=c.match(/^group = (\d+)/m);j.frames.push(n);if(g)j.groups[n]=+g[1];if(/^done = no/m.test(c))left.push(n)}}
 bj.set(x.id,j);noFile();if(ended(x.state)){j.closed=1;if(x.state!=="done")for(const n of left)failed.add(n)}
 // the job followed here already (its replayed lines may come before its
 // file): its windows' frames now known
 if(task&&task.id===x.id){groups=j.groups;dofs=j.dofs;if(run.kind==="window")scanning=new Set(Object.keys(groups).filter(n=>groups[n]===run.g).map(Number).filter(n=>!landed.includes(n)||rescan.has(n)))}}
// adopted ended jobs without a file, once the frames are known (a
// reload's first snapshot may come before them)
function noFile(){if(!frames.length)return;for(const j of bj.values())if(j.nofile===1){j.nofile=2;
 for(const f of frames)if(!blocked(f)&&dec[f.n]==="confirmed"&&!landed.includes(f.n)&&!jobOf(f.n))failed.add(f.n)}}
const ended=s=>/^(done|failed|stopped)$/.test(s);
// the open job a frame is in: queued, running, or ended with its end not
// handled here yet (its windows landing)
const jobOf=n=>[...bj.values()].find(j=>!j.closed&&j.frames.includes(n));
// full: a POST got 507; live: the box's queued|running job ids, as the
// last snapshot had them
let startT=0,startMs=3000,bq=Promise.resolve(),full=0,live=new Set();
const todoFrames=()=>frames.filter(f=>!blocked(f)&&dec[f.n]==="confirmed"&&!jobOf(f.n)&&!failed.has(f.n)&&
 (!landed.includes(f.n)&&!unfetched.has(f.n)||rescan.has(f.n))).map(f=>f.n);
// each decision starts the countdown again; at its end the confirmed
// frames not in a job go to the batch. Not while this strip's preview is
// open (its frames still coming): its end starts the countdown
function maybeStart(){if(!frames.length)return;if([...bj.values()].some(j=>j.op==="preview"&&!j.closed))return;const todo=todoFrames();if(!todo.length)return;
 if(!scanPx)return st("no resolution for this strip (strip.scan)",1);clearTimeout(startT);
 $("tt").textContent=([...bj.values()].some(j=>!j.closed&&j.op==="scan")?"adding ":"starting the scan of ")+todo.length+" frame"+(todo.length>1?"s":"")+" …";$("toast").hidden=false;
 startT=setTimeout(()=>{startT=0;$("toast").hidden=true;enqueue(todoFrames())},startMs)}
function cancelStart(){if(!startT)return;clearTimeout(startT);startT=0;$("toast").hidden=true;st("scan not started: confirm or skip a frame to start it")}
// frames -> the batch: merged into this strip's tail job while it is
// still queued (PUT its job), else a job of their own (POST); a rescan
// is a job of its own. One change to the batch at a time (bq); a frame
// no longer confirmed when its turn comes stays out
const enqueue=todo=>bq=bq.then(async()=>{todo=todo.filter(n=>!jobOf(n)&&dec[n]==="confirmed");if(!todo.length)return;
 const re=todo.filter(n=>rescan.has(n)),add=todo.filter(n=>!rescan.has(n));
 if(re.length)await post(re,1);
 if(add.length){const t=tail();if(!t||!await put(t,t.frames.concat(add).sort((a,b)=>a-b)))await post(add)}
 jobs();for(const f of frames)badge(f)}).catch(e=>st(e.message,1));
// this strip's last scan job, if it is still queued (and not a rescan)
function tail(){let t=null;for(const j of bj.values())if(j.op==="scan"&&(!t||j.id>t.id))t=j;return t&&t.state==="queued"&&!t.rescan?t:null}
// frames ns planned (the rest skipped): the job's text, its windows'
// groups and depth of field. One pixel count per window: a cropped or
// partial frame needs more ppi for it, and its window scans at that;
// said, not hidden
async function planFor(ns){const r=await ft("plan",frames.map(f=>ns.includes(f.n)?scanPx:0)),j=await ft("jobJson",meta),gs={},ds={};
 for(const f of j.frames)if(f.group)gs[f.n]=f.group,ds[f.group]=f.dof;
 const top=Math.max(...j.frames.map(f=>f.ppi)),why=ns.filter(n=>cropped.has(n));
 $("ppin").textContent=top>+info.ppi+1?"note: this scan runs at "+top+" ppi, not "+info.ppi+": every frame gets "+scanPx+
  " px on its long side"+(why.length?", and frame"+(why.length>1?"s ":" ")+why.join(", ")+" (cropped) need"+(why.length>1?"":"s")+" more ppi for that":""):"";
 return{text:r.text,groups:gs,dofs:ds}}
// a new job at the batch's tail; a full batch (507) keeps the frames
// back until a job ends
async function post(ns,re){const p=await planFor(ns),r=await fetch("/api/tasks",form({op:"scan",strip:cur,share:1,job:p.text}));
 const o=await r.json().catch(()=>({}));
 if(r.status===507){full=1;st("the batch is full: frame"+(ns.length>1?"s ":" ")+ns.join(", ")+" wait for a job to end",1);return null}
 if(!r.ok)throw new Error(o.error||r.status+" "+r.statusText);
 const j={id:o.id,op:"scan",state:"queued",frames:ns.slice(),groups:p.groups,dofs:p.dofs,rescan:re?1:0};bj.set(j.id,j);return j}
// a queued job's frames replaced (its job file); false if it has
// started meanwhile (409)
async function put(j,ns){const p=await planFor(ns),r=await fetch("/api/tasks/"+j.id+"/job",{method:"PUT",body:p.text});
 if(r.status===409){j.state="running";return false}
 if(!r.ok){const o=await r.json().catch(()=>({}));throw new Error(o.error||r.status+" "+r.statusText)}
 Object.assign(j,{frames:ns.slice(),groups:p.groups,dofs:p.dofs});return true}

// the batch (SSE, a snapshot per change): its jobs' states; a job of
// this strip that runs is followed; one that ended unseen is replayed
// from its first line (the box keeps its lines) for its windows. After
// a 507 the frames wait until a job leaves the queued|running set (ended
// or dropped: a slot free), not for any snapshot
function batchWatch(){if(bes)bes.close();bes=new EventSource("/api/batch/events");bes.onmessage=e=>onBatch(JSON.parse(e.data))}
const onBatch=b=>bq=bq.then(async()=>{const s=cur,top=Math.max(0,...b.jobs.map(x=>x.id)),
  now=new Set(b.jobs.filter(x=>!ended(x.state)).map(x=>x.id)),freed=[...live].some(id=>!now.has(id));live=now;
 for(const x of b.jobs)if(x.strip===s)await adopt(x);if(s!==cur)return;
 for(const x of b.jobs){if(x.strip!==cur)continue;const j=bj.get(x.id);if(!j)continue;j.state=x.state;
  if(x.state==="running"&&(!task||task.id!==x.id))watch({id:x.id,state:"running",strip:x.strip,op:x.op});
  else if(ended(x.state)&&!j.closed&&!j.watched)watch({id:x.id,state:x.state,strip:x.strip,op:x.op})}
 // queued jobs gone from the box (dropped); a snapshot older than a job
 // just posted does not list it yet
 for(const[id,j]of bj)if(j.state==="queued"&&id<top&&!b.jobs.some(x=>x.id===id))bj.delete(id);
 jobs();for(const f of frames)badge(f);if(full&&freed){full=0;maybeStart()}}).catch(e=>st(e.message,1));

// tasks: start (a preview), follow (SSE: a stream per job, from its
// first line to its end), stop
async function start(o){o.strip=cur;const r=await api("/api/tasks",form(o));$("pb").style.width="0";
 bj.set(r.id,{id:r.id,op:o.op,state:"queued",frames:[],groups:{},dofs:{}});watch({id:r.id,state:"running",strip:cur,op:o.op})}
function busy(b){$("stop").disabled=!b;$("again").hidden=true}
const ess=new Map();
function watch(t){const j=bj.get(t.id);if(t.state==="running"){if(!task||task.id!==t.id)$("log").textContent="";task=t;busy(true)}
 if(ess.has(t.id)||j&&j.watched)return;if(j)j.watched=1;
 const es=new EventSource("/api/tasks/"+t.id+"/events?since=0");ess.set(t.id,es);es.onmessage=e=>onLine(e.data,t.id,t.op);
 const end=()=>{es.close();ess.delete(t.id);const k=bj.get(t.id);if(k&&!k.exited)done(-1,t.id,t.op)};
 es.addEventListener("end",end);es.addEventListener("gone",end)}
function onLine(t,id,op){const mine=!!task&&task.id===id;
 if(mine){const lg=$("log");lg.textContent+=t+"\n";if(lg.textContent.length>2e4)lg.textContent=lg.textContent.slice(-15e3)}
 const w=t.split(" ");if(w[0]!=="progress"){if(t.startsWith("ftwebd exit")){const j=bj.get(id);if(j)j.exited=1;done(+w[2],id,op)}return}
 if(w[1]==="file"){const m=t.match(/[\\/]([^\\/]+)[\\/](strip_(?:j\d+_)?win\d\d\.raw)$/);if(m&&m[1]===cur&&op==="scan")land(m[2]);return}
 if(!mine)return;                       // a job replayed after its end: its files only
 if(w[1]==="begin"){const j=bj.get(id);if(j){groups=j.groups;dofs=j.dofs}run={kind:w[2],n:+w[3]||1,i:1};fzReset();st(w[2]==="upload"?"the scanner was powered up: uploading its microcode first …":w[2]+" …")}
 else if(w[1]==="unit"){run.i=+w[3];run.g=+w[4];fzReset();if(run.kind==="window"){const g=+w[4];scanning=new Set(Object.keys(groups).filter(n=>groups[n]===g).map(Number).filter(n=>!landed.includes(n)||rescan.has(n)));for(const f of frames)badge(f);jobs()}st(run.kind+" "+run.i+"/"+run.n)}
 else if(w[1]==="lines"){const[k,n]=w[2].split("/").map(Number);$("pb").style.width=((run.i-1+k/n)/run.n*100).toFixed(1)+"%";st(run.kind+" "+run.i+"/"+run.n+" · line "+k+"/"+n);
  if(op==="preview")liveGrow(n)}
 else if(/^(focus|focused|sharp)$/.test(w[1]))onFocus(w);
 else if(w[1]==="end"){if(w[2]==="ok")$("pb").style.width="100%";st(run.kind+" "+w[2],w[2]!=="ok")}}
let landQ=Promise.resolve();
const land=n=>landQ=landQ.then(async()=>{const before=landed.slice(),was=new Set(rescan);await cut(n);
 const nw=landed.filter(x=>!before.includes(x)||was.has(x)&&!rescan.has(x));for(const x of nw)scanning.delete(x);
 await reprint();thumbs(nw);jobs();if(nw.includes(sel))pick(sel)}).catch(e=>st(e.message,1));
// a job's end, once its windows landed: its frames that did not land
// (and were not only left unfetched) are to retry. A preview followed
// here: its streamed frames sealed (else, a replay, the strip reloaded);
// strip.job, if held back while a preview ran, written now
async function done(code,id,op){const s=cur,mine=!!task&&task.id===id;if(mine){busy(false);task.state="done"}
 if(mine&&code!==0&&/ftpoc: (no microcode file|the microcode upload failed|the microcode did not answer)/.test($("log").textContent))st("the scanner is in its loader (after a power-up) and its microcode upload failed: see the log (MICROCOD.3XX beside ftpoc.exe, or ftwebd --microcode)",1);
 else if(mine&&code!==0&&!/stopped|ok/.test($("rst").textContent))st("failed (exit "+code+"); see the log",1);
 await landQ;if(s!==cur)return;const j=bj.get(id);
 if(j&&!j.closed){j.closed=1;if(!ended(j.state))j.state=code===0?"done":"failed";
  for(const n of j.frames){scanning.delete(n);if((!landed.includes(n)||rescan.has(n))&&!unfetched.has(n))failed.add(n)}}
 if(op==="preview"){if(mine)await previewDone(code===0);else await load();noFile();maybeStart()}
 else{if(jobDirty)await putJob();for(const f of frames)badge(f);jobs();maybeStart()}}

// focus: ftpoc's crops as the status, the frame and a sharpness plot; a
// share window's peak shaded +-DOF95/2 (its frames' dof, ftw_dof95): how
// far it may drift (or be carried) and stay at >= 95 % sharpness
let fz=[],fzAt=null;
function fzReset(){fz=[];fzAt=null;$("fz").hidden=true}
function onFocus(w){const u=run.kind+" "+run.i+"/"+run.n+" · ";
 if(w[1]==="focus")st(u+"focusing"+(w[4]?" frame "+w[4]:"")+" · crop "+w[2]+(w[6]?" at "+w[6]:""));
 else if(w[1]==="sharp"){fz.push([+w[2],+w[3]]);plot()}
 else{const h=run.kind==="window"&&w[3]!=="none"&&dofs[run.g]?Math.round(dofs[run.g]/2):0;
  fzAt=[+w[2],w[2]+" "+w[3],h];st(u+(w[3]==="carried"?"focus "+w[2]+" carried: no crops":w[3]==="none"?"no focus peak: "+w[2]+" as it was":"focus "+w[2]+" ("+w[3]+")")+(h?" · DOF ±"+h+" steps":""),w[3]==="none");plot()}}
function plot(){const c=$("fz"),d=devicePixelRatio,W=240*d,H=64*d,P=8*d;c.hidden=false;c.width=W;c.height=H;const x=c.getContext("2d"),
 h=fzAt?fzAt[2]:0,os=fz.map(p=>p[0]).concat(fzAt?[fzAt[0]-h,fzAt[0]+h]:[]),lo=Math.min(...os)-250,hi=Math.max(...os)+250,ss=fz.map(p=>p[1]),a=Math.min(...ss),b=Math.max(...ss),
 X=o=>P+(o-lo)/(hi-lo)*(W-2*P),Y=v=>H-P-(b>a?(v-a)/(b-a):.5)*(H-3*P);
 x.fillStyle="#171311";x.fillRect(0,0,W,H);if(h){x.fillStyle="#7fd17f26";x.fillRect(X(fzAt[0]-h),P,X(fzAt[0]+h)-X(fzAt[0]-h),H-2*P)}x.lineWidth=d;x.strokeStyle="#ff9a1f88";x.beginPath();
 [...fz].sort((p,q)=>p[0]-q[0]).forEach((p,i)=>i?x.lineTo(X(p[0]),Y(p[1])):x.moveTo(X(p[0]),Y(p[1])));x.stroke();x.fillStyle="#ff9a1f";
 for(const p of fz){x.beginPath();x.arc(X(p[0]),Y(p[1]),2.5*d,0,7);x.fill()}
 if(fzAt){const m=X(fzAt[0]);x.strokeStyle="#7fd17f";x.beginPath();x.moveTo(m,P);x.lineTo(m,H-P);x.stroke()}}

// downloads
function save(name,bytes,type){const a=document.createElement("a");a.href=URL.createObjectURL(new Blob([bytes],{type}));a.download=name;document.body.append(a);a.click();a.remove();setTimeout(()=>URL.revokeObjectURL(a.href),6e4)}
async function download(){const n=sel;if(!landed.includes(n))return;try{st("printing frame "+n+" …");const r=await ft("jpeg",n),nm=(await ft("name",n))||autoName(n);
 save(nm+".jpg",r.jpg,"image/jpeg");save(nm+".jpg.txt",r.text,"text/plain");st("")}catch(e){st(e.message,1)}}
async function zipAll(){try{st("printing every frame …");save(cur+".zip",await ft("zip",cur),"application/zip");st("")}catch(e){st(e.message,1)}}

// keymaps: presets matched on the typed character (a key's shifted
// symbol beside it); the same physical block in every preset
const COMMON={arrowleft:"prev",arrowright:"next",home:"first",end:"last",enter:"confirm",x:"skip",backspace:"undo","0":"reset",escape:"esc","?":"keys","/":"keys"};
const KEYMAPS={
 qwerty:{q:"d+",a:"d-",w:"c+",s:"c-",e:"m+",d:"m-",r:"y+",f:"y-",t:"left",g:"right",n:"name",z:"zoom"},
 dvorak:{"'":"d+",'"':"d+",a:"d-",",":"c+","<":"c+",o:"c-",".":"m+",">":"m+",e:"m-",p:"y+",u:"y-",y:"left",i:"right",b:"name",f:"zoom"}};
const ACT={"c+":"C +","c-":"C −","m+":"M +","m-":"M −","y+":"Y +","y-":"Y −","d+":"D +","d-":"D −",left:"turn left",right:"turn right",name:"edit the name",zoom:"zoom 1:1 / fit",
 prev:"previous frame",next:"next frame",first:"first frame",last:"last frame",confirm:"confirm, next",skip:"skip, next",undo:"back to pending",reset:"reset C/M/Y/D",esc:"close / leave",keys:"this keymap"};
const preset=()=>KEYMAPS[ls("keys")]?ls("keys"):"qwerty";
function act(a){const m=a.match(/^([cmyd])([+-])$/);if(m)return click("cmyd".indexOf(m[1]),m[2]==="+"?1:-1);
 ({prev:()=>step(-1),next:()=>step(1),first:()=>frames[0]&&pick(frames[0].n),last:()=>frames.length&&pick(frames[frames.length-1].n),
   confirm:()=>decide("confirmed"),skip:()=>decide("skipped"),undo:()=>decide("pending"),reset,left:()=>turn(-90),right:()=>turn(90),
   name:()=>$("name").focus(),zoom:toggleZoom,keys:keymap,esc:()=>{if(!$("km").hidden)return keymap();if(startT)cancelStart()}})[a]()}
function keymap(){const k=$("km");if(!k.hidden){k.hidden=true;return}const p=preset(),m=Object.assign({},KEYMAPS[p]),rows=[];
 const by={};for(const[key,a]of Object.entries(Object.assign({},COMMON,m)))(by[a]=by[a]||[]).push(key.length>1?key[0].toUpperCase()+key.slice(1):key.toUpperCase());
 for(const a of Object.keys(ACT))if(by[a])rows.push("<tr><td>"+by[a].map(x=>"<kbd>"+x.replace(/</g,"&lt;")+"</kbd>").join(" ")+"</td><td>"+ACT[a]+"</td></tr>");
 k.innerHTML='<div class="row"><b>Keys</b><select id="kp" aria-label="Keyboard layout">'+Object.keys(KEYMAPS).map(x=>"<option"+(x===p?" selected":"")+">"+x+"</option>").join("")+'</select><button id="kx">Close (Esc)</button></div><table>'+rows.join("")+"</table>";
 $("kp").onchange=()=>{ls("keys",$("kp").value);k.hidden=true;keymap()};$("kx").onclick=keymap;k.hidden=false}
function onKey(e){if(location.hash.slice(1,3)!=="s="||e.ctrlKey||e.metaKey||e.altKey||$("dlg").open)return;
 if(e.target===$("name")){if(e.key==="Enter"||e.key==="Escape"){setName();e.target.blur();e.preventDefault()}return}
 if(/INPUT|SELECT|TEXTAREA/.test(e.target.tagName))return;
 const k=e.key.length===1?e.key.toLowerCase():e.key.toLowerCase(),a=KEYMAPS[preset()][k]||COMMON[k];if(!a)return;e.preventDefault();act(a)}
async function toggleZoom(){zoomed=!zoomed;$("bw").className=zoomed?"z":"";$("zm").textContent=zoomed?"fit":"1:1";bigPrint=null;await drawBig()}

function wire(){
 $("live").onclick=e=>{const c=$("live"),r=c.getBoundingClientRect(),x=(e.clientX-r.left)*c.width/r.width,y=(e.clientY-r.top)*c.height/r.height;
  const f=frames.find(f=>x>=f.x&&x<=f.x+f.w&&y>=f.y&&y<=f.y+f.h);if(f)pick(f.n)};
 const b=$("big");b.onpointerdown=onDown;b.onpointermove=onMove;b.onpointerup=onUp;b.onpointercancel=()=>{drag=null;drawBig()};
 $("name").onchange=setName;$("tl").onclick=()=>turn(-90);$("tr").onclick=()=>turn(90);$("zm").onclick=toggleZoom;
 $("prev").onclick=()=>step(-1);$("next").onclick=()=>step(1);$("ok").onclick=()=>decide("confirmed");$("skip").onclick=()=>decide("skipped");
 $("dl").onclick=download;$("all").onclick=zipAll;$("tc").onclick=cancelStart;$("clr").onclick=clearQ;$("kh").onclick=e=>{e.preventDefault();keymap()};
 $("stop").onclick=async()=>{try{await api("/api/tasks/"+task.id+"/stop",{method:"POST"});cancelStart();st("stopping at the next line …")}catch(e){st(e.message,1)}};
 $("again").onclick=async()=>{try{await start({op:"preview",film:filmOf(emOf())});liveReset();await streamStart()}catch(e){st(e.message,1)}};
 addEventListener("keydown",onKey);addEventListener("resize",()=>{if(sel&&!zoomed){bigPrint=null;drawBig()}})}
