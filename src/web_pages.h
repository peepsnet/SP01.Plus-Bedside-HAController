#pragma once
#include <Arduino.h>

// Layout editor served at http://bedside.local/  (GUESS: not browser-tested on a device yet)
static const char EDITOR_HTML[] PROGMEM = R"HTML(<!doctype html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Bedside layout</title>
<style>
body{font:14px system-ui,sans-serif;background:#16181b;color:#e6e8ea;margin:0;padding:12px}
a{color:#7ab}
#wrap{display:flex;flex-wrap:wrap;gap:16px}
#canvas{position:relative;width:720px;height:420px;background-color:#101214;border:1px solid #444;touch-action:none;
 background-image:linear-gradient(#2a2e33 1px,transparent 1px),linear-gradient(90deg,#2a2e33 1px,transparent 1px);background-size:30px 30px}
.b{position:absolute;box-sizing:border-box;border-radius:12px;display:flex;align-items:center;justify-content:center;
 text-align:center;color:#fff;cursor:move;user-select:none;font-size:15px;overflow:hidden}
.b.sl{background:#23272b!important;flex-direction:column;align-items:flex-start;justify-content:flex-start;padding:8px}
.b.sl i{display:block;height:8px;border-radius:4px;margin-top:auto;margin-bottom:10px;width:100%;opacity:.8}
.b.sel{outline:2px solid #fff}
.h{position:absolute;right:0;bottom:0;width:18px;height:18px;background:#fff8;cursor:nwse-resize}
#form{width:280px}
label{display:block;margin:6px 0 2px;color:#9aa}
input,select,button{width:100%;box-sizing:border-box;padding:6px;background:#23262a;color:#eee;border:1px solid #444;border-radius:6px}
button{cursor:pointer;margin-top:6px}
.tab{display:inline-block;width:auto;padding:6px 10px;margin-right:4px}
.tab.on{background:#3d7bd9}
small{color:#9aa}
</style></head><body>
<h3>Bedside layout editor &nbsp; <small><a href="/settings">Settings</a> &middot; <a href="/update">Update</a> &middot; <span id="ver"></span></small></h3>
<div id="tabs"></div>
<div id="wrap">
<div id="canvas"></div>
<div id="form">
 <button id="add">+ Add button</button><button id="addsl">+ Add slider</button>
 <div id="props" style="display:none">
  <label>Label</label><input id="label">
  <label>Entity <small id="entnote"></small></label><input id="entity_id" list="ents" placeholder="light.bedside" autocomplete="off">
  <datalist id="ents"></datalist>
  <button id="reload">Reload entity list from Home Assistant</button>
  <label>Size and position, in grid cells (1 cell = 20 px)</label>
  <div id="cells" style="display:grid;grid-template-columns:1fr 1fr;gap:6px">
   <span><small>Width</small><input id="cw" type="number" min="2" step="1"></span>
   <span><small>Height</small><input id="ch" type="number" min="2" step="1"></span>
   <span><small>Column</small><input id="cx" type="number" min="0" step="1"></span>
   <span><small>Row</small><input id="cy" type="number" min="0" step="1"></span>
  </div>
  <div id="sizeinfo"><small></small></div>
  <div id="btnprops">
   <label>Action</label><select id="action"></select>
   <label>Icon</label><select id="icon"></select>
  </div>
  <div id="slprops">
   <label>Slider controls</label><select id="attr"><option value="">Default for this device</option><option value="brightness">Light brightness</option><option value="color_temp">Light color temperature</option></select>
   <label>Min (blank = default)</label><input id="min" type="number">
   <label>Max (blank = default)</label><input id="max" type="number">
  </div>
  <label>Color</label><input id="color" type="color">
  <button id="del">Delete</button>
 </div>
 <hr><button id="addpage">+ Add page</button><button id="renpage">Rename page</button><button id="delpage">Delete page</button>
 <button id="save" style="background:#2d7a3e">Save &amp; apply</button>
 <div id="msg"></div>
</div></div>
<script>
const $=id=>document.getElementById(id);
fetch('/api/version').then(r=>r.text()).then(v=>$('ver').textContent='v'+v);
const S=1.5,W=480,H=280,G=20;  // G = grid cell in screen pixels (24 x 14 cells)
const cells=v=>Math.round(v/G*10)/10;
const ICONS=['','power','eye','home','play','pause','bell','settings','ok','wifi','audio','charge','image'];
const ACTIONS=[['toggle','Toggle'],['on','Turn on'],['off','Turn off'],['scene','Activate scene'],['script','Run script'],['press','Press button'],['open','Cover: open'],['close','Cover: close'],['stop','Cover: stop'],['lock','Lock'],['unlock','Unlock'],['play_pause','Media: play/pause']];
let L={pages:[]},pi=0,sel=null;
ICONS.forEach(i=>$('icon').add(new Option(i||'(none)',i)));
ACTIONS.forEach(a=>$('action').add(new Option(a[1],a[0])));
fetch('/api/layout').then(r=>r.json()).then(j=>{L=j;if(!L.pages.length)L.pages=[{name:'Page 1',buttons:[]}];draw()});
function loadEnts(refresh){
 fetch('/api/entities'+(refresh?'?refresh=1':'')).then(r=>r.json()).then(j=>{
  $('ents').innerHTML='';j.entities.forEach(e=>{const o=document.createElement('option');o.value=e[0];o.label=e[1];$('ents').appendChild(o)});
  $('entnote').textContent=j.connected?(j.entities.length+' devices'+(j.loading?' (loading...)':'')+' via '+j.endpoint):'(Home Assistant not connected; type the id)';
  if(j.loading)setTimeout(()=>loadEnts(false),1500)});
}
loadEnts(false);
$('reload').onclick=()=>loadEnts(true);
function draw(){
 $('tabs').innerHTML='';
 L.pages.forEach((p,i)=>{const t=document.createElement('button');t.className='tab'+(i==pi?' on':'');t.textContent=p.name;t.onclick=()=>{pi=i;sel=null;draw()};$('tabs').appendChild(t)});
 const c=$('canvas');c.innerHTML='';
 const bs=L.pages[pi].buttons;
 bs.forEach((b,i)=>{
  const d=document.createElement('div');const sl=b.type=='slider';d.className='b'+(sl?' sl':'')+(sel===i?' sel':'');
  d.style.cssText+=`left:${b.x*S}px;top:${b.y*S}px;width:${b.w*S}px;height:${b.h*S}px;background:${b.color||'#4A90E2'}`;
  const t=document.createElement('span');t.textContent=b.label||b.entity_id||'';d.appendChild(t);
  if(sl){const bar=document.createElement('i');bar.style.background=b.color||'#4A90E2';d.appendChild(bar)}
  d.onpointerdown=e=>{sel=i;drag(e,b,'move');showProps();markSel()};
  const h=document.createElement('div');h.className='h';h.onpointerdown=e=>{e.stopPropagation();sel=i;drag(e,b,'size')};
  d.appendChild(h);c.appendChild(d)});
 showProps();
}
function markSel(){[...$('canvas').children].forEach((d,i)=>d.classList.toggle('sel',i===sel))}
function drag(e,b,mode){
 e.preventDefault();const sx=e.clientX,sy=e.clientY,ox=b.x,oy=b.y,ow=b.w,oh=b.h,el=e.currentTarget.classList.contains('h')?e.currentTarget.parentNode:e.currentTarget;
 const mv=ev=>{const dx=Math.round((ev.clientX-sx)/S/G)*G,dy=Math.round((ev.clientY-sy)/S/G)*G;
  if(mode=='move'){b.x=Math.max(0,Math.min(W-b.w,ox+dx));b.y=Math.max(0,Math.min(H-b.h,oy+dy))}
  else{b.w=Math.max(2*G,Math.min(W-b.x,ow+dx));b.h=Math.max(2*G,Math.min(H-b.y,oh+dy))}
  el.style.left=b.x*S+'px';el.style.top=b.y*S+'px';el.style.width=b.w*S+'px';el.style.height=b.h*S+'px';cellInfo(b)};
 const up=()=>{removeEventListener('pointermove',mv);removeEventListener('pointerup',up)};
 addEventListener('pointermove',mv);addEventListener('pointerup',up);
}
const FIELDS=['label','icon','entity_id','action','color','attr','min','max'];
function cellInfo(b){
 $('cw').value=cells(b.w);$('ch').value=cells(b.h);$('cx').value=cells(b.x);$('cy').value=cells(b.y);
 $('sizeinfo').firstChild.textContent=cells(b.w)+' x '+cells(b.h)+' cells  ('+b.w+' x '+b.h+' px)';
}
function applyCells(){
 const b=L.pages[pi].buttons[sel];if(!b)return;
 const v=id=>Math.round(+$(id).value*G);
 b.w=Math.max(2*G,Math.min(W,v('cw')));b.h=Math.max(2*G,Math.min(H,v('ch')));
 b.x=Math.max(0,Math.min(W-b.w,v('cx')));b.y=Math.max(0,Math.min(H-b.h,v('cy')));
 const el=$('canvas').children[sel];
 el.style.left=b.x*S+'px';el.style.top=b.y*S+'px';el.style.width=b.w*S+'px';el.style.height=b.h*S+'px';
 $('sizeinfo').firstChild.textContent=cells(b.w)+' x '+cells(b.h)+' cells  ('+b.w+' x '+b.h+' px)';
}
for(const id of['cw','ch','cx','cy'])$(id).onchange=applyCells;
function showProps(){
 const b=sel!=null?L.pages[pi].buttons[sel]:null;$('props').style.display=b?'block':'none';if(!b)return;
 const sl=b.type=='slider';$('btnprops').style.display=sl?'none':'block';$('slprops').style.display=sl?'block':'none';
 for(const k of FIELDS)$(k).value=b[k]!==undefined?b[k]:(k=='color'?'#4A90E2':'');
 cellInfo(b);
}
for(const k of FIELDS)$(k).oninput=()=>{
 const b=L.pages[pi].buttons[sel];
 if((k=='min'||k=='max')){if($(k).value==='')delete b[k];else b[k]=+$(k).value}else b[k]=$(k).value;
 const d=$('canvas').children[sel];
 if(k=='label'||k=='entity_id')d.firstChild.textContent=b.label||b.entity_id;
 if(k=='color'){d.style.background=b.color;const i=d.querySelector('i');if(i)i.style.background=b.color}};
$('add').onclick=()=>addW({type:'button',label:'New',icon:'power',x:20,y:20,w:140,h:80,action:'toggle'});
$('addsl').onclick=()=>addW({type:'slider',label:'Brightness',x:20,y:20,w:200,h:80});
function addW(o){const bs=L.pages[pi].buttons;bs.push(Object.assign({id:'b'+Date.now(),entity_id:'',color:'#4A90E2'},o));sel=bs.length-1;draw()}
$('del').onclick=()=>{L.pages[pi].buttons.splice(sel,1);sel=null;draw()};
$('addpage').onclick=()=>{L.pages.push({name:'Page '+(L.pages.length+1),buttons:[]});pi=L.pages.length-1;sel=null;draw()};
$('renpage').onclick=()=>{const n=prompt('Page name',L.pages[pi].name);if(n){L.pages[pi].name=n;draw()}};
$('delpage').onclick=()=>{if(L.pages.length>1&&confirm('Delete page?')){L.pages.splice(pi,1);pi=0;sel=null;draw()}};
$('save').onclick=async()=>{const r=await fetch('/api/layout',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(L)});
 $('msg').textContent=r.ok?'Saved and applied.':'Save failed: '+await r.text()};
</script></body></html>)HTML";

static const char SETTINGS_HTML[] PROGMEM = R"HTML(<!doctype html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Bedside settings</title>
<style>body{font:15px system-ui,sans-serif;background:#16181b;color:#e6e8ea;padding:14px;max-width:420px}
label{display:block;margin:10px 0 3px;color:#9aa}input,button{width:100%;box-sizing:border-box;padding:8px;background:#23262a;color:#eee;border:1px solid #444;border-radius:6px}
a{color:#7ab}</style></head><body>
<h3>Settings &nbsp; <small><a href="/">Layout</a> &middot; <a href="/update">Update</a></small></h3>
<form id="f">
<label>Wi-Fi SSID</label><input name="wifi_ssid">
<label>Wi-Fi password (leave blank to keep)</label><input name="wifi_pass" type="password">
<h4>Home Assistant, local address (tried first)</h4>
<label>Host</label><input name="ha_host" placeholder="homeassistant.local or 192.168.1.10">
<label>Port</label><input name="ha_port" type="number" value="8123">
<label><input type="checkbox" name="ha_tls" style="width:auto"> Use TLS (wss)</label>
<h4>External address (fallback when local is unreachable, optional)</h4>
<label>Host</label><input name="ha_ext_host" placeholder="myname.duckdns.org or xxxx.ui.nabu.casa">
<label>Port</label><input name="ha_ext_port" type="number" value="443">
<label><input type="checkbox" name="ha_ext_tls" style="width:auto" checked> Use TLS (wss)</label>
<h4>Screen brightness range (used by the swipe-down slider)</h4>
<label>Minimum % (5 to 90)</label><input name="bright_min" type="number" min="5" max="90" value="10">
<label>Maximum % (at least 10 above minimum, up to 100)</label><input name="bright_max" type="number" min="15" max="100" value="100">
<label>Long-lived access token (leave blank to keep)</label><input name="ha_token" type="password">
<button type="submit" style="margin-top:14px;background:#2d7a3e">Save &amp; restart</button>
</form><div id="msg"></div>
<script>
const f=document.getElementById('f');
fetch('/api/config').then(r=>r.json()).then(j=>{for(const k in j){const e=f.elements[k];if(!e)continue;if(e.type=='checkbox')e.checked=j[k];else e.value=j[k]}});
f.onsubmit=async e=>{e.preventDefault();const o={};for(const el of f.elements){if(!el.name)continue;o[el.name]=el.type=='checkbox'?el.checked:el.type=='number'?+el.value:el.value}
 const r=await fetch('/api/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(o)});
 document.getElementById('msg').textContent=r.ok?'Saved. Device is restarting...':'Save failed'};
</script></body></html>)HTML";

static const char UPDATE_HTML[] PROGMEM = R"HTML(<!doctype html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Bedside update</title>
<style>body{font:15px system-ui,sans-serif;background:#16181b;color:#e6e8ea;padding:14px;max-width:420px}
input,button{width:100%;box-sizing:border-box;padding:8px;margin-top:10px;background:#23262a;color:#eee;border:1px solid #444;border-radius:6px}a{color:#7ab}</style></head><body>
<h3>Firmware update &nbsp; <small><a href="/">Layout</a></small></h3>
<p>Running: <b id="ver">...</b></p>
<p>Upload <code>firmware.bin</code> from a GitHub release (or <code>.pio/build/wt32-sc01-plus/firmware.bin</code>). The device reboots when done.</p>
<input type="file" id="f" accept=".bin"><button id="go" style="background:#2d7a3e">Upload &amp; install</button>
<progress id="p" value="0" max="100" style="width:100%;margin-top:10px"></progress><div id="msg"></div>
<script>
const $=id=>document.getElementById(id);
fetch('/api/version').then(r=>r.text()).then(v=>$('ver').textContent='v'+v);
$('go').onclick=()=>{const f=$('f').files[0];if(!f)return;const x=new XMLHttpRequest(),fd=new FormData();fd.append('firmware',f);
 x.upload.onprogress=e=>$('p').value=e.loaded/e.total*100;
 x.onload=()=>$('msg').textContent=x.status==200?'Installed. Rebooting...':'Failed: '+x.responseText;
 x.onerror=()=>$('msg').textContent='Connection lost (may be rebooting).';
 x.open('POST','/update');x.send(fd)};
</script></body></html>)HTML";
