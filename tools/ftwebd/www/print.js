// SPDX-License-Identifier: GPL-2.0-or-later
// The page's print stage (index.html loads it deferred: nothing here
// is needed before frames land): cut each finished window in the
// engine, the strip's balance, the grid and the large view, C/M/Y/D
// clicks (buttons, keys c m y d, Shift: minus, arrows: frames) and the
// downloads; each frame's turn (buttons, keys r R, the strip's own ↻);
// the large view's zoom (tap, z) and the strip preview's (- +); the strip
// as prints (pos); what focus is doing. Shares index.html's globals ($,
// ft, api, cur, ...).
"use strict";
// a finished window: its files -> the engine's cut
async function cut(name,quiet){const s=cur,[raw,cal,meta,faul]=await Promise.all([get(F(name),1),get(F(name+".cal"),1),get(F(name+".txt"),1),get(F(name+".faul"),1)]);
 if(s!==cur)return;if(!raw||!cal||!meta)return quiet||st(name+": files missing",1);
 try{const r=await ft("cut",raw,cal,meta,faul);for(const f of r.frames)if(!landed.includes(f.frame))landed.push(f.frame);landed.sort((a,b)=>a-b);if(!sel)sel=landed[0]}
 catch(e){if(!quiet)st(name+": "+e.message,1)}}
// the strip's balance over every landed frame, then the screen copies
async function reprint(){if(!landed.length)return;try{$("notes").textContent=await ft("measure")}catch(e){return st(e.message,1)}
 shown.clear();const g=$("grid");g.innerHTML="";for(const f of landed){const c=document.createElement("canvas");c.id="t"+f;c.title="frame "+f;c.onclick=()=>pick(f);g.append(c)}
 await pick(sel||landed[0]);for(const f of landed)if(!shown.has(f))await show(f,0)}
// the large view fits the screen, or (zoomed: tap it, z) shows the print
// at one image pixel per device pixel
let zoomed=0;
const bigEdge=()=>zoomed?2e4:Math.min(1600,Math.round(innerWidth*devicePixelRatio));
async function show(f,big){const d=await ft("display",f,big?bigEdge():320),c=big?$("big"):$("t"+f);
 if(!c)return;c.width=d.w;c.height=d.h;c.getContext("2d").putImageData(new ImageData(new Uint8ClampedArray(d.rgba.buffer),d.w,d.h),0,0);
 if(big){c.style.width=zoomed?d.w/devicePixelRatio+"px":"";const t=$("t"+f),s=Math.min(1,320/Math.max(d.w,d.h));
  if(t){t.width=Math.round(d.w*s);t.height=Math.round(d.h*s);t.getContext("2d").drawImage(c,0,0,t.width,t.height)}}shown.set(f,1)}
async function zoom(e){if(!sel)return;const c=$("big"),r=c.getBoundingClientRect(),m=e instanceof MouseEvent&&r.width,
 fx=m?(e.clientX-r.left)/r.width:.5,fy=m?(e.clientY-r.top)/r.height:.5,b=$("bw");
 zoomed=!zoomed;b.className=zoomed?"z":"";if(zoomed)st("zooming …");await show(sel,1);st("");
 if(zoomed){b.scrollLeft=fx*b.scrollWidth-b.clientWidth/2;b.scrollTop=fy*b.scrollHeight-b.clientHeight/2}}
$("big").onclick=zoom;
function draw(k){$("keys").innerHTML="";$("dl").disabled=$("all").disabled=!k;if(!k){const c=$("big");c.width=c.height=0;return}
 "CMYD".split("").forEach((n,i)=>{const s=document.createElement("span");s.className="k";
  s.innerHTML=n+' <button>−</button><b>'+(k[i]>0?"+":"")+k[i]+'</b><button>+</button>';
  const b=s.querySelectorAll("button");b[0].onclick=()=>click(i,-1);b[1].onclick=()=>click(i,1);$("keys").append(s)});
 const t=document.createElement("span");t.className="k";
 t.innerHTML='<button title="turn left (Shift+R)">↺</button><button title="turn right (R)">↻</button><button title="every frame turned as this one">↻ all</button>';
 const b=t.querySelectorAll("button");b[0].onclick=()=>turn(-90);b[1].onclick=()=>turn(90);b[2].onclick=turnAll;$("keys").append(t)}
async function pick(f){sel=f;for(const c of $("grid").children)c.className=c.id==="t"+f?"sel":"";kk[f]=await ft("printGet",f);draw(kk[f]);await show(f,1)}
// one click after another (fast keys must not read stale clicks)
let clickQ=Promise.resolve();
const click=(i,d)=>clickQ=clickQ.then(async()=>{if(!sel)return;const f=sel,k=kk[f].slice();k[i]+=d;
 try{await ft("clicks",f,k);kk[f]=k;if(f===sel){draw(k);await show(f,1)}savePrint()}catch(e){st(e.message,1)}});
// the print's turn (strip.print rotate): the screen copies, the JPEGs
const turn=d=>clickQ=clickQ.then(async()=>{if(!sel)return;const f=sel,r=((await ft("rotation",f))+d+360)%360;
 try{await ft("rotate",f,r);savePrint();await show(f,f===sel);await turned(f,r)}catch(e){st(e.message,1)}});
const turnAll=()=>clickQ=clickQ.then(async()=>{if(!sel)return;
 try{const r=await ft("rotation",sel);for(const f of landed)if(f!==sel&&await ft("rotation",f)!==r){await ft("rotate",f,r);await show(f,0);await turned(f,r)}savePrint()}catch(e){st(e.message,1)}});
// the strip's ↻: the same turn, before or after the scan
const turnFrame=fr=>clickQ=clickQ.then(async()=>{const r=((await ft("rotation",fr.n))+90)%360;
 try{await ft("rotate",fr.n,r);savePrint();await turned(fr.n,r);if(landed.includes(fr.n))await show(fr.n,fr.n===sel)}catch(e){st(e.message,1)}});
function savePrint(){clearTimeout(putT);putT=setTimeout(async()=>{try{await api(S("strip.print"),{method:"PUT",body:await ft("printText")})}catch(e){st(e.message,1)}},400)}
function save(name,bytes,type){const a=document.createElement("a");a.href=URL.createObjectURL(new Blob([bytes],{type}));a.download=name;document.body.append(a);a.click();a.remove();setTimeout(()=>URL.revokeObjectURL(a.href),6e4)}
$("em").onchange=async()=>{const n=await ft("printEmulsion",$("em").value);if(n)st(n+" frame(s) back to auto: their clicks do not fit");savePrint();pvImg.clear();paintStrip();await reprint()};
$("dl").onclick=async()=>{try{st("printing frame "+sel+" …");const r=await ft("jpeg",sel),n=cur+"_frame"+String(sel).padStart(2,"0")+".jpg";save(n,r.jpg,"image/jpeg");save(n+".txt",r.text,"text/plain");st("")}catch(e){st(e.message,1)}};
$("all").onclick=async()=>{try{st("printing every frame …");save(cur+".zip",await ft("zip",cur),"application/zip");st("")}catch(e){st(e.message,1)}};
addEventListener("keydown",e=>{if(/INPUT|SELECT/.test(e.target.tagName)||!sel||e.ctrlKey||e.metaKey||e.altKey)return;const i="cmyd".indexOf(e.key.toLowerCase()),j=landed.indexOf(sel);
 if(i>=0)click(i,e.key===e.key.toLowerCase()?1:-1);else if(e.key==="r"||e.key==="R")turn(e.key==="r"?90:-90);else if(e.key==="z")zoom(e);else if(e.key==="ArrowRight"&&j<landed.length-1)pick(landed[j+1]);else if(e.key==="ArrowLeft"&&j>0)pick(landed[j-1]);else return;e.preventDefault()});
let landing=Promise.resolve();
const land=n=>landing=landing.then(async()=>{await cut(n);await reprint()});
// the strip preview's zoom (the frame boxes are in %: they follow)
const ZS=[1,1.5,2,3,4];let sz=+ls("zoom")||1;
function szoom(d){const b=$("sv").parentNode,c=(b.scrollLeft+b.clientWidth/2)/b.scrollWidth;
 sz=ZS[Math.max(0,Math.min(ZS.length-1,ZS.indexOf(sz)+d))];ls("zoom",""+sz);
 Object.assign($("sv").style,{width:sz>1?"calc("+sz*100+"% - 20px)":"",minWidth:640*sz+"px"});
 b.scrollLeft=c*b.scrollWidth-b.clientWidth/2;$("zo").disabled=sz===1;$("zi").disabled=sz===ZS[ZS.length-1];if(frames.length)paintStrip()}
$("zo").onclick=()=>szoom(-1);$("zi").onclick=()=>szoom(1);szoom(0);
// the strip preview's frames: each turned as strip.print turns it and
// (pos) printed from the preview's own raw by the engine: the strip's
// balance, the emulsion, no clicks. Off and unturned: strip.png itself.
let pos=ls("pos")==="1",pvOk=0,pvLoad=null,psQ=Promise.resolve();const pvImg=new Map();
function pvReset(){pvOk=0;pvLoad=null;pvImg.clear()}
async function pvFiles(){const[raw,cal,meta,faul]=await Promise.all([get(F("strip.raw"),1),get(F("strip.raw.cal"),1),get(F("strip.raw.txt")),get(F("strip.raw.faul"),1)]);
 if(!raw||!cal||!meta)throw new Error("no preview raw");await ft("previewLoad",raw,cal,meta,faul)}
async function pvSource(fr){if(!pos||!pvOk){const i=$("sv").querySelector("img");return[i,fr.x,fr.y,fr.w,fr.h]}
 if(!pvImg.has(fr.n)){const d=await ft("previewDisplay",fr.n,4000),c=document.createElement("canvas");c.width=d.w;c.height=d.h;
  c.getContext("2d").putImageData(new ImageData(new Uint8ClampedArray(d.rgba.buffer),d.w,d.h),0,0);pvImg.set(fr.n,c)}
 const c=pvImg.get(fr.n);return[c,0,0,c.width,c.height]}
async function frameDraw(fr){const b=fr.el,r=fr.rot||0;if(!b)return;const c=b.querySelector("canvas");
 if(!(pos&&pvOk)&&!r)return c.style.display="none";const[im,sx,sy,sw,sh]=await pvSource(fr);
 const W=Math.max(1,Math.round(b.clientWidth*devicePixelRatio)),H=Math.max(1,Math.round(b.clientHeight*devicePixelRatio)),k=r%180?Math.min(W/sh,H/sw):Math.min(W/sw,H/sh),x=c.getContext("2d");
 c.width=W;c.height=H;x.fillStyle="#000";x.fillRect(0,0,W,H);x.translate(W/2,H/2);x.rotate(r*Math.PI/180);x.drawImage(im,sx,sy,sw,sh,-sw*k/2,-sh*k/2,sw*k,sh*k);c.style.display="block"}
async function paintStrip_(){const s=cur;$("pn").disabled=!pvOk;$("pn").setAttribute("aria-pressed",pos&&pvOk);$("sv").classList.toggle("pos",pos&&pvOk);
 for(const f of frames)f.rot=await ft("rotation",f.n).catch(()=>0);
 if(pos&&pvOk){pvLoad=pvLoad||pvFiles();try{await pvLoad}catch(e){pvLoad=null;st("pos: "+e.message,1);pos=0;return paintStrip_()}}
 for(const f of frames)if(s===cur)await frameDraw(f).catch(e=>st("pos: "+e.message,1))}
const paintStrip=()=>psQ=psQ.then(paintStrip_).catch(e=>st(e.message,1));
async function turned(n,r){const f=frames.find(x=>x.n===n);if(f){f.rot=r;await frameDraw(f)}}
$("pn").onclick=()=>{pos=!pos;ls("pos",pos?"1":"0");paintStrip()};
// focus: ftpoc's crops (progress focus K frame F at O, sharp O S, focused
// O how) as the status, the frame pulsing on the strip and a plot of
// sharpness against the offset with the offset taken; a share window's
// peak shaded +-DOF95/2 (its frames' dof, ftw_dof95): how far it may
// drift (or be carried) and stay at >= 95 % sharpness. Not for an ARC's
// job (task.arc: after its frames --detail step): not strip.job's windows
let fz=[],fzAt=null;
function fzFrame(n){for(const f of frames)if(f.el)f.el.toggleAttribute("data-fz",f.n===n)}
function fzReset(){fz=[];fzAt=null;fzFrame(0);$("fz").hidden=true}
function onFocus(w){const u=run.kind+" "+run.i+"/"+run.n+" · ";
 if(w[1]==="focus"){fzFrame(+w[4]||0);st(u+"focusing"+(w[4]?" frame "+w[4]:"")+" · crop "+w[2]+(w[6]?" at "+w[6]:""))}
 else if(w[1]==="sharp"){fz.push([+w[2],+w[3]]);plot()}
 else{const g=run.kind==="window"&&!task.arc&&w[3]!=="none"&&frames.find(f=>f.group===run.g&&f.dof),h=g?Math.round(g.dof/2):0;
  fzAt=[+w[2],w[2]+" "+w[3],h];fzFrame(0);st(u+(w[3]==="carried"?"focus "+w[2]+" carried: no crops":w[3]==="none"?"no focus peak: "+w[2]+" as it was":"focus "+w[2]+" ("+w[3]+")")+(h?" · DOF ±"+h+" steps":""),w[3]==="none");plot()}}
function plot(){const c=$("fz"),d=devicePixelRatio,W=240*d,H=64*d,P=8*d;c.hidden=false;c.width=W;c.height=H;const x=c.getContext("2d"),
 h=fzAt?fzAt[2]:0,os=fz.map(p=>p[0]).concat(fzAt?[fzAt[0]-h,fzAt[0]+h]:[]),lo=Math.min(...os)-250,hi=Math.max(...os)+250,ss=fz.map(p=>p[1]),a=Math.min(...ss),b=Math.max(...ss),
 X=o=>P+(o-lo)/(hi-lo)*(W-2*P),Y=v=>H-P-(b>a?(v-a)/(b-a):.5)*(H-3*P);
 x.fillStyle="#171311";x.fillRect(0,0,W,H);if(h){x.fillStyle="#7fd17f26";x.fillRect(X(fzAt[0]-h),P,X(fzAt[0]+h)-X(fzAt[0]-h),H-2*P)}x.lineWidth=d;x.strokeStyle="#ff9a1f88";x.beginPath();
 [...fz].sort((p,q)=>p[0]-q[0]).forEach((p,i)=>i?x.lineTo(X(p[0]),Y(p[1])):x.moveTo(X(p[0]),Y(p[1])));x.stroke();x.fillStyle="#ff9a1f";
 for(const p of fz){x.beginPath();x.arc(X(p[0]),Y(p[1]),2.5*d,0,7);x.fill()}
 x.font=10*d+"px "+getComputedStyle(document.body).fontFamily;x.fillStyle="#8a7d70";x.fillText(fz.length?"sharpness":"no crops",P,P+4*d);
 if(fzAt){const m=X(fzAt[0]);x.strokeStyle="#7fd17f";x.beginPath();x.moveTo(m,P);x.lineTo(m,H-P);x.stroke();x.fillStyle="#7fd17f";x.textAlign=m>W/2?"right":"left";x.fillText(fzAt[1],m+(m>W/2?-4:4)*d,H-P)}}
