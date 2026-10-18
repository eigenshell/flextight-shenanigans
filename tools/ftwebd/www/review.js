// SPDX-License-Identifier: GPL-2.0-or-later
// The review screen (index.html loads it deferred): the live preview bar,
// then the strip's frames one at a time (a positive of each, its name,
// crop, turn and C/M/Y/D, confirm or skip), the job list (the scan starts
// once no frame is pending; a crop on a frame already scanned asks for a
// rescan), the prints as frames land, the downloads and the keymaps
// (qwerty, dvorak). Shares index.html's globals ($, ft, api, get, cur,
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
.b.confirmed,.b.queued{color:var(--amber)}.b.scanning{color:#ffd04a}.b.done{color:var(--ok)}.b.skipped,.b.under{color:var(--dim)}
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
<div class="row"><span class="st" id="rst"></span><button id="again" hidden>Preview again</button><button id="stop" class="stop" disabled>Stop</button></div>
<div class="note"><span id="fet"></span> <span id="fbad"></span></div>
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
let frames=[],dec={},landed=[],sel=0,task=null,es=null,run={kind:"",n:1,i:1},scanPx=0,info={},meta="";
let pv=null,pvImg=null,kk={},queued=new Set(),scanning=new Set(),rescan=new Set(),cropped=new Set(),groups={},zoomed=0;
// frames of a window scanned but not fetched (no rescan for them; a
// reload fetches again), and why, per window
let unfetched=new Set(),fe={};
const NN=n=>String(n).padStart(2,"0"),frame=n=>frames.find(f=>f.n===n);
const autoName=n=>cur+"_frame"+NN(n);
// partial and half frames: under construction. A window keeps one pixel
// count, so their shorter long side would raise the ppi of their whole
// run; they wait for per-frame resolution
const UC="under construction",blocked=f=>f.type==="partial"||f.type==="half";
function stateOf(f){return blocked(f)?UC:scanning.has(f.n)?"scanning":queued.has(f.n)?"queued":landed.includes(f.n)&&!rescan.has(f.n)?"done":dec[f.n]||"pending"}
const started=f=>/queued|scanning|done/.test(stateOf(f));

// ---------------------------------------------------------------- screen
async function review(s){
 if(!$("live")){const st_=document.createElement("style");st_.textContent=CSS;document.head.append(st_);$("rv").innerHTML=HTML;wire()}
 frames=[];dec={};landed=[];sel=0;kk={};queued.clear();scanning.clear();rescan.clear();cropped.clear();groups={};pv=null;pvImg=null;unfetched.clear();fe={};fetchBad();fetching("");
 $("thumbs").innerHTML="";$("jobs").innerHTML="";$("notes").textContent="";$("ppin").textContent="";$("log").textContent="";$("pb").style.width="0";
 $("again").hidden=true;drawBig();
 const t=await api("/api/tasks/current").catch(()=>null);
 info=parseKV(await get(S("files/strip.scan"))||"");
 scanPx=0;if(info.ppi)try{scanPx=(await ft("resolutions",[+info.ppi]))[0].px}catch(e){st(e.message,1)}
 if(t&&t.state==="running"&&t.strip===s){watch(t);if(t.op==="preview"){liveReset();return}}
 await load()}
function parseKV(t){const o={};for(const l of t.split("\n")){const m=l.match(/^(\w+) = (.*)$/);if(m)o[m[1]]=m[2]}return o}
const emOf=()=>info.emulsion||(ems.find(e=>e.mode==="negative")||ems[0]).id;

// the strip's files -> frames, decisions, prints
async function load(){const s=cur;
 const pj=await get(F("strip.preview.job"))||await get(S("strip.job"));if(s!==cur)return;
 if(!pj){$("again").hidden=!!(task&&task.state==="running");if(!$("rst").classList.contains("err"))st("no preview yet");return}
 try{await ft("job",pj);const j=await get(S("strip.job"));if(j)await ft("planLoad",j)}catch(e){return st(e.message,1)}
 const p=await get(S("strip.print"));
 try{if(p)await ft("printLoad",p);else await ft("printNew",emOf())}catch(e){st("strip.print: "+e.message,1);await ft("printNew",emOf())}
 meta=await get(F("strip.raw.txt"))||"";const j=await ft("jobJson",meta);frames=j.frames;
 dec={};for(const l of(await get(S("strip.review"))||"").split("\n")){const m=l.match(/^(\d+) (\w+)$/);if(m)dec[+m[1]]=m[2]}
 const[raw,cal,faul]=await Promise.all([get(F("strip.raw"),1),get(F("strip.raw.cal"),1),get(F("strip.raw.faul"),1)]);
 if(s!==cur)return;
 if(raw&&cal&&meta){pv=[raw,cal,meta,faul];try{await ft("previewLoad",...pv)}catch(e){pv=null;st("preview: "+e.message,1)}}
 pvImg=await img(F("strip.png")+"?t="+Date.now());
 // the frames from the preview at once; scanned windows (tens of MB each)
 // are cut after, each frame redrawn as its print lands
 drawStrip();jobs();const first=frames.find(f=>stateOf(f)==="pending")||frames[0];
 await thumbs();if(first)await pick(first.n);st("");
 const l=await api(S("files")).catch(()=>[]),wins=winFiles(l.map(x=>x.name));
 for(const[i,n]of wins.entries())landQ=landQ.then(async()=>{if(s!==cur)return;st("loading scanned window "+(i+1)+"/"+wins.length+" …");
  const before=landed.slice();await cut(n,1);const nw=landed.filter(x=>!before.includes(x));if(s!==cur)return;
  await reprint();await thumbs(landed.slice());jobs();drawStrip();if(landed.includes(sel))await pick(sel);if(i===wins.length-1)st("")}).catch(e=>st(e.message,1))}
const img=src=>new Promise(y=>{const i=new Image();i.onload=()=>y(i);i.onerror=()=>y(null);i.src=src});
// window scans, earlier runs first (strip_rK_winNN), the latest last
function winFiles(names){const k=n=>{const m=n.match(/^strip_(?:r(\d+)_)?win(\d\d)\.raw$/);return m?[m[1]?+m[1]:1e9,+m[2]]:null};
 return names.filter(k).sort((a,b)=>{const x=k(a),y=k(b);return x[0]-y[0]||x[1]-y[1]})}
// a scanned window: its raw streamed (fetchBig), then cut. Never throws:
// a window not fetched is said (#fbad) and its frames wait for a reload
async function cut(name,quiet){const s=cur;let fr=[],mb="";try{
 const[cal,mt,faul]=await Promise.all([get(F(name+".cal"),1),get(F(name+".txt"),1),get(F(name+".faul"),1)]);
 if(s!==cur)return;if(!cal||!mt)return quiet||st(name+": files missing",1);
 fr=(new TextDecoder().decode(mt).match(/^share_frame=\d+/gm)||[]).map(x=>+x.slice(12));
 const raw=await fetchBig(F(name),(k,n)=>{if(s!==cur)return 0;const m=MB(k)+"/"+MB(n);if(m!==mb){mb=m;fetching(name+": fetching "+m+" MB")}return 1});
 if(s!==cur)return;if(!raw)return quiet||st(name+": files missing",1);
 for(const x of fr)unfetched.delete(x);delete fe[name];fetchBad();
 try{const r=await ft("cut",raw,cal,mt,faul);for(const f of r.frames){if(!landed.includes(f.frame))landed.push(f.frame);rescan.delete(f.frame)}landed.sort((a,b)=>a-b)}
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
async function liveGrow(n){if(lv.bad||lv.busy||Date.now()-lv.at<900)return;lv.busy=1;lv.at=Date.now();
 try{if(!lv.px)lv.px=await ft("previewPx");const c=$("live");if(c.width!==n){c.width=n;c.height=lv.px;lv.n=n}
  const r=await fetch(F("strip.raw"),{headers:{Range:"bytes="+lv.off+"-"}});if(r.status===416)return;
  if(r.status!==206)throw new Error(r.status);const b=new Uint8Array(await r.arrayBuffer());lv.off+=b.length;
  const all=new Uint8Array(lv.rest.length+b.length);all.set(lv.rest);all.set(b,lv.rest.length);
  const L=lv.px*6,k=Math.floor(all.length/L);lv.rest=all.slice(k*L);if(!k)return;
  const d=await ft("liveRows",all.subarray(0,k*L),lv.px,filmOf(emOf())!=="slide",lv.first);lv.first=0;
  const o=new OffscreenCanvas(d.w,d.h);o.getContext("2d").putImageData(new ImageData(new Uint8ClampedArray(d.rgba.buffer),d.w,d.h),0,0);
  const x=c.getContext("2d");x.setTransform(0,1,1,0,0,0);x.drawImage(o,0,lv.line);x.setTransform(1,0,0,1,0,0);lv.line+=d.h}
 catch(e){lv.bad=1}finally{lv.busy=0}}
function drawStrip(){const c=$("live"),i=pvImg;if(!i)return;c.width=i.naturalWidth;c.height=i.naturalHeight;const x=c.getContext("2d");x.drawImage(i,0,0);
 for(const f of frames){const s=stateOf(f);x.lineWidth=f.n===sel?8:4;x.strokeStyle=f.n===sel?"#ff9a1f":s==="done"?"#7fd17f":s==="skipped"||blocked(f)?"#5a4e44":"#fff8";x.strokeRect(f.x,f.y,f.w,f.h)}}

// thumbnails: each frame as it will print (scanned: the scan's print)
async function thumbs(only){const t=$("thumbs");if(!only){t.innerHTML="";for(const f of frames){const b=document.createElement("button");b.className="th";b.id="th"+f.n;
  b.innerHTML='<canvas></canvas><span class="n">'+f.n+'</span><span class="b"></span>';b.onclick=()=>pick(f.n);t.append(b)}}
 for(const f of frames)if(!only||only.includes(f.n)){badge(f);await paintTo($("th"+f.n).querySelector("canvas"),f.n,240).catch(()=>{})}}
function badge(f){const b=$("th"+f.n);if(!b)return;const s=stateOf(f);b.classList.toggle("sel",f.n===sel);const e=b.querySelector(".b");e.className="b "+s;e.textContent=s}
async function paintTo(c,n,edge){const d=landed.includes(n)?await ft("display",n,edge):pv?await ft("previewPrint",n,edge):null;if(!d)return null;
 c.width=d.w;c.height=d.h;c.getContext("2d").putImageData(new ImageData(new Uint8ClampedArray(d.rgba.buffer),d.w,d.h),0,0);return c}

// the large view: the print in its box, the strip around it, the crop
let G=null,drag=null,bigPrint=null;
async function drawBig(){const c=$("big"),f=frame(sel);if(!f){c.width=c.height=0;return}
 const bw=$("bw"),dpr=devicePixelRatio,r=await ft("rotation",f.n).catch(()=>0);
 if(zoomed){bigPrint=await paintTo(document.createElement("canvas"),f.n,2e4);if(!bigPrint)return;c.width=bigPrint.width;c.height=bigPrint.height;c.style.width=c.width/dpr+"px";c.getContext("2d").drawImage(bigPrint,0,0);G=null;return}
 c.style.width="";const b=f,m=0.12*Math.max(b.w,b.h),I=pvImg,iw=I?I.naturalWidth:b.x+b.w+m,ih=I?I.naturalHeight:b.y+b.h+m;
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
// a crop: the job's window, kept (strip.preview.job); a frame already in a
// scan asks first
async function crop(f,b){
 if(blocked(f)){st("frame "+f.n+" ("+f.type+"): "+UC,1);return drawBig()}
 if(started(f)&&!rescan.has(f.n)){const a=await ask("Frame "+f.n+" is already "+(stateOf(f)==="done"?"scanned":stateOf(f))+". A new crop needs a rescan.");
  if(a!=="rescan")return drawBig();rescan.add(f.n);dec[f.n]="confirmed";saveReview()}
 try{const t=await ft("frameBox",f.n,[b.x,b.y,b.w,b.h],meta);await api(S("strip.preview.job"),{method:"PUT",body:t});
  Object.assign(f,{x:b.x,y:b.y,w:b.w,h:b.h});cropped.add(f.n);if(pv)await ft("previewLoad",...pv);bigPrint=null;await drawBig();drawStrip();thumbs([f.n]);jobs();maybeStart()}
 catch(e){st(e.message,1);drawBig()}}
const ask=q=>new Promise(y=>{const d=$("dlg");$("dq").textContent=q;d.showModal();const done=v=>{d.close();d.onkeydown=null;y(v)};
 $("dr").onclick=()=>done("rescan");$("dk").onclick=()=>done("keep");$("dx").onclick=()=>done("cancel");d.oncancel=e=>{e.preventDefault();done("cancel")};
 d.onkeydown=e=>{const k=e.key.toLowerCase();if(k==="r")done("rescan");else if(k==="k")done("keep")}});

// one frame selected: its controls
async function pick(n){const f=frame(n);if(!f)return;sel=n;bigPrint=null;for(const g of frames)badge(g);
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
 if(started(f)&&d==="pending")return st("frame "+f.n+" is "+stateOf(f)+": its decision stays",1);
 dec[f.n]=d;saveReview();badge(f);jobs();drawStrip();
 if(d!=="pending"){const i=frames.indexOf(f),nx=frames.slice(i+1).concat(frames.slice(0,i)).find(g=>stateOf(g)==="pending");if(nx)pick(nx.n);else pick(f.n)}
 maybeStart()}
function step(d){const i=frames.findIndex(f=>f.n===sel)+d;if(i>=0&&i<frames.length)pick(frames[i].n)}

// the job list; the scan starts when no frame is pending (phase 1)
function jobs(){const t=$("jobs");t.innerHTML="<tr><th>Frame</th><th>Name</th><th>State</th></tr>";
 for(const f of frames){const r=t.insertRow();r.insertCell().textContent=f.n;r.insertCell().textContent=autoName(f.n);r.insertCell().textContent=stateOf(f);
  ft("name",f.n).then(v=>{if(v)r.cells[1].textContent=v}).catch(()=>{})}}
let startT=0;
function maybeStart(){if(task&&task.state==="running")return;if(!frames.length||frames.some(f=>stateOf(f)==="pending"))return;
 const todo=frames.filter(f=>!blocked(f)&&dec[f.n]==="confirmed"&&(!landed.includes(f.n)&&!unfetched.has(f.n)||rescan.has(f.n))).map(f=>f.n);if(!todo.length||startT)return;
 if(!scanPx)return st("no resolution for this strip (strip.scan)",1);
 $("tt").textContent="starting the scan of "+todo.length+" frame"+(todo.length>1?"s":"")+" …";$("toast").hidden=false;
 startT=setTimeout(()=>{startT=0;$("toast").hidden=true;scanFrames(todo)},3000)}
function cancelStart(){if(!startT)return;clearTimeout(startT);startT=0;$("toast").hidden=true;st("scan not started: confirm or skip a frame to start it")}
async function scanFrames(todo){try{const r=await ft("plan",frames.map(f=>todo.includes(f.n)?scanPx:0));await api(S("strip.job"),{method:"PUT",body:r.text});
 const j=await ft("jobJson",meta);groups={};for(const f of j.frames)if(f.group)groups[f.n]=f.group;
 // one pixel count per window: a cropped or partial frame needs more ppi
 // for it, and its window scans at that; said, not hidden
 const top=Math.max(...j.frames.map(f=>f.ppi)),why=frames.filter(f=>todo.includes(f.n)&&cropped.has(f.n)).map(f=>f.n);
 queued=new Set(todo);await start({op:"scan",share:1});jobs();for(const f of frames)badge(f);
 $("ppin").textContent=top>+info.ppi+1?"note: this scan runs at "+top+" ppi, not "+info.ppi+": every frame gets "+scanPx+
  " px on its long side"+(why.length?", and frame"+(why.length>1?"s ":" ")+why.join(", ")+" (cropped) need"+(why.length>1?"":"s")+" more ppi for that":""):""}
 catch(e){queued.clear();st(e.message,1)}}

// tasks: start, follow (SSE), stop
async function start(o){o.strip=cur;const r=await api("/api/tasks",form(o));$("pb").style.width="0";watch({id:r.id,state:"running",strip:cur,op:o.op})}
function busy(b){$("stop").disabled=!b;$("again").hidden=true}
function watch(t){task=t;busy(t.state==="running");if(es)es.close();$("log").textContent="";
 es=new EventSource("/api/tasks/"+t.id+"/events?since=0");es.onmessage=e=>onLine(e.data);
 const end=()=>{es.close();es=null};es.addEventListener("end",end);es.addEventListener("gone",end)}
function onLine(t){const lg=$("log");lg.textContent+=t+"\n";if(lg.textContent.length>2e4)lg.textContent=lg.textContent.slice(-15e3);
 const w=t.split(" ");if(w[0]!=="progress"){if(t.startsWith("ftwebd exit"))done(+w[2]);return}
 if(w[1]==="begin"){run={kind:w[2],n:+w[3]||1,i:1};fzReset();st(w[2]==="upload"?"the scanner was powered up: uploading its microcode first …":w[2]+" …")}
 else if(w[1]==="unit"){run.i=+w[3];fzReset();if(run.kind==="window"){const g=+w[4];scanning=new Set(Object.keys(groups).filter(n=>groups[n]===g).map(Number));for(const f of frames)badge(f);jobs()}st(run.kind+" "+run.i+"/"+run.n)}
 else if(w[1]==="lines"){const[k,n]=w[2].split("/").map(Number);$("pb").style.width=((run.i-1+k/n)/run.n*100).toFixed(1)+"%";st(run.kind+" "+run.i+"/"+run.n+" · line "+k+"/"+n);
  if(task&&task.op==="preview")liveGrow(n)}
 else if(/^(focus|focused|sharp)$/.test(w[1]))onFocus(w);
 else if(w[1]==="file"){const m=t.match(/[\\/]([^\\/]+)[\\/](strip_win\d\d\.raw)$/);if(m&&m[1]===cur&&task.op==="scan")land(m[2])}
 else if(w[1]==="end"){if(w[2]==="ok")$("pb").style.width="100%";st(run.kind+" "+w[2],w[2]!=="ok")}}
let landQ=Promise.resolve();
const land=n=>landQ=landQ.then(async()=>{const before=landed.slice();await cut(n);const nw=landed.filter(x=>!before.includes(x)||queued.has(x));
 for(const x of nw){queued.delete(x);scanning.delete(x)}await reprint();thumbs(nw);jobs();if(nw.includes(sel))pick(sel)}).catch(e=>st(e.message,1));
async function done(code){busy(false);const op=task&&task.op;if(task)task.state="done";scanning.clear();queued.clear();
 if(code!==0&&/ftpoc: (no microcode file|the microcode upload failed|the microcode did not answer)/.test($("log").textContent))st("the scanner is in its loader (after a power-up) and its microcode upload failed: see the log (MICROCOD.3XX beside ftpoc.exe, or ftwebd --microcode)",1);
 else if(code!==0&&!/stopped|ok/.test($("rst").textContent))st("failed (exit "+code+"); see the log",1);
 await landQ;if(!task||task.strip!==cur)return;if(op==="preview")await load();else{for(const f of frames)badge(f);jobs();maybeStart()}}

// focus: ftpoc's crops as the status, the frame and a sharpness plot
let fz=[],fzAt=null;
function fzReset(){fz=[];fzAt=null;$("fz").hidden=true}
function onFocus(w){const u=run.kind+" "+run.i+"/"+run.n+" · ";
 if(w[1]==="focus")st(u+"focusing"+(w[4]?" frame "+w[4]:"")+" · crop "+w[2]+(w[6]?" at "+w[6]:""));
 else if(w[1]==="sharp"){fz.push([+w[2],+w[3]]);plot()}
 else{fzAt=[+w[2],w[2]+" "+w[3]];st(u+(w[3]==="carried"?"focus "+w[2]+" carried: no crops":w[3]==="none"?"no focus peak: "+w[2]+" as it was":"focus "+w[2]+" ("+w[3]+")"),w[3]==="none");plot()}}
function plot(){const c=$("fz"),d=devicePixelRatio,W=240*d,H=64*d,P=8*d;c.hidden=false;c.width=W;c.height=H;const x=c.getContext("2d"),
 os=fz.map(p=>p[0]).concat(fzAt?[fzAt[0]]:[]),lo=Math.min(...os)-250,hi=Math.max(...os)+250,ss=fz.map(p=>p[1]),a=Math.min(...ss),b=Math.max(...ss),
 X=o=>P+(o-lo)/(hi-lo)*(W-2*P),Y=v=>H-P-(b>a?(v-a)/(b-a):.5)*(H-3*P);
 x.fillStyle="#171311";x.fillRect(0,0,W,H);x.lineWidth=d;x.strokeStyle="#ff9a1f88";x.beginPath();
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
 $("dl").onclick=download;$("all").onclick=zipAll;$("tc").onclick=cancelStart;$("kh").onclick=e=>{e.preventDefault();keymap()};
 $("stop").onclick=async()=>{try{await api("/api/tasks/"+task.id+"/stop",{method:"POST"});cancelStart();st("stopping at the next line …")}catch(e){st(e.message,1)}};
 $("again").onclick=async()=>{try{await start({op:"preview",film:filmOf(emOf())});liveReset()}catch(e){st(e.message,1)}};
 addEventListener("keydown",onKey);addEventListener("resize",()=>{if(sel&&!zoomed){bigPrint=null;drawBig()}})}
