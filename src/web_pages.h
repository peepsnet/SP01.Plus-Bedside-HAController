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
#canvas{position:relative;background-color:#101214;border:1px solid #444;touch-action:none;
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
<div style="margin:6px 0">Editing the <select id="orient" style="width:auto"><option value="landscape">Landscape layout (480 x 280)</option><option value="portrait">Portrait layout (320 x 440)</option></select> <small id="orientnote"></small>
 <div style="margin-top:6px">Screen rotation: <select id="rot" style="width:auto"><option value="0">0 (landscape)</option><option value="1">90 clockwise (portrait)</option><option value="2">180 (landscape upside down)</option><option value="3">90 counter-clockwise (portrait)</option></select> <button id="rotgo" style="width:auto;display:inline-block;margin:0 0 0 6px">Rotate screen &amp; restart</button> <small>The layout selector above only chooses which layout you are editing, it does not turn the screen.</small></div></div>
<div id="tabs"></div>
<div id="wrap">
<div id="canvas"></div>
<div id="form">
 <button id="add">+ Add button</button><button id="addsl">+ Add slider</button><button id="addcam">+ Add camera button</button>
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
   <div id="actwrap"><label>Action</label><select id="action"></select></div>
   <label>Icon</label><select id="icon"></select>
   <div id="slwrap"><label style="margin-top:10px"><input id="withslider" type="checkbox" style="width:auto"> Also show a slider for this entity</label>
   <small id="slnote"></small></div>
  </div>
  <div id="camprops" style="display:none">
   <label>IP camera URL <small>(or pick a Home Assistant camera entity above)</small></label>
   <input id="url" placeholder="http://user:pass@192.168.1.50/mjpeg" autocomplete="off">
   <label><input id="fullbright" type="checkbox" style="width:auto"> Force full screen brightness while the camera is open</label>
   <small>Tapping the button opens the camera full screen; tap or swipe to close. Needs an MJPEG or JPEG snapshot URL. RTSP is not supported.</small>
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
let S=1.5,W=480,H=280;const G=20;  // G = grid cell in screen pixels
let orient='landscape';
const cells=v=>Math.round(v/G*10)/10;
const ICONS=['','power','eye','home','play','pause','bell','settings','ok','wifi','audio','charge','image','video'];
const ACTIONS=[['toggle','Toggle'],['on','Turn on'],['off','Turn off'],['scene','Activate scene'],['script','Run script'],['press','Press button'],['open','Cover: open'],['close','Cover: close'],['stop','Cover: stop'],['lock','Lock'],['unlock','Unlock'],['play_pause','Media: play/pause']];
let L={pages:[]},pi=0,sel=null;
ICONS.forEach(i=>$('icon').add(new Option(i||'(none)',i)));
ACTIONS.forEach(a=>$('action').add(new Option(a[1],a[0])));
function loadLayout(){
 W=orient=='portrait'?320:480;H=orient=='portrait'?440:280;
 const c=$('canvas');c.style.width=W*S+'px';c.style.height=H*S+'px';
 fetch('/api/layout?o='+orient).then(r=>r.json()).then(j=>{L=j;pi=0;sel=null;if(!L.pages.length)L.pages=[{name:'Page 1',buttons:[]}];draw()});
}
$('rotgo').onclick=async()=>{if(!confirm('Rotate the screen and restart the device?'))return;await fetch('/api/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({rotation:+$('rot').value})});$('msg').textContent='Restarting with the new rotation...'};
fetch('/api/config').then(r=>r.json()).then(j=>{orient=(j.rotation&1)?'portrait':'landscape';$('orient').value=orient;$('rot').value=j.rotation;
 $('orientnote').textContent='Screen is currently '+(orient=='portrait'?'portrait':'landscape')+'. Each orientation has its own layout.';loadLayout()}).catch(loadLayout);
$('orient').onchange=()=>{if(confirm('Switch layout? Unsaved changes to this one are lost.')){orient=$('orient').value;loadLayout()}else $('orient').value=orient};
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
 const bs=L.pages[pi].buttons,lk=mode=='move'?bs.map((x,i)=>x.linked===b.id?i:-1).filter(i=>i>=0):[],lo=lk.map(i=>[bs[i].x,bs[i].y]);
 const mv=ev=>{const dx=Math.round((ev.clientX-sx)/S/G)*G,dy=Math.round((ev.clientY-sy)/S/G)*G;
  if(mode=='move'){b.x=Math.max(0,Math.min(W-b.w,ox+dx));b.y=Math.max(0,Math.min(H-b.h,oy+dy))}
  else{b.w=Math.max(2*G,Math.min(W-b.x,ow+dx));b.h=Math.max(2*G,Math.min(H-b.y,oh+dy))}
  el.style.left=b.x*S+'px';el.style.top=b.y*S+'px';el.style.width=b.w*S+'px';el.style.height=b.h*S+'px';cellInfo(b);lk.forEach((i,n)=>{const t=bs[i];t.x=Math.max(0,Math.min(W-t.w,lo[n][0]+b.x-ox));t.y=Math.max(0,Math.min(H-t.h,lo[n][1]+b.y-oy));const e=$('canvas').children[i];e.style.left=t.x*S+'px';e.style.top=t.y*S+'px'})};
 const up=()=>{removeEventListener('pointermove',mv);removeEventListener('pointerup',up)};
 addEventListener('pointermove',mv);addEventListener('pointerup',up);
}
const FIELDS=['label','icon','entity_id','action','color','attr','min','max','url'];
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
const SLIDER_DOMAINS=['light','cover','fan','media_player','climate','number','input_number'];
const domainOf=b=>(b.entity_id||'').split('.')[0];
const linkedIdx=b=>L.pages[pi].buttons.findIndex(x=>x.linked===b.id);
function updateSliderBox(b){
 const ok=SLIDER_DOMAINS.includes(domainOf(b));
 $('withslider').disabled=!ok;$('withslider').checked=linkedIdx(b)>=0;
 $('slnote').textContent=!b.entity_id?'Pick an entity first.':(ok?'':'No slider for this device type.');
}
$('withslider').onchange=()=>{
 const bs=L.pages[pi].buttons,b=bs[sel];if(!b)return;
 if($('withslider').checked){
  // Place the slider right under the button, else above it, else at the bottom of the screen.
  const w=Math.max(b.w,4*G),h=3*G;let x=Math.min(b.x,W-w),y=b.y+b.h;
  if(y+h>H){y=b.y-h;if(y<0)y=Math.max(0,H-h)}
  bs.push({id:'s'+Date.now(),type:'slider',label:b.label,entity_id:b.entity_id,color:b.color,x,y,w,h,linked:b.id});
 }else{
  const i=linkedIdx(b);if(i>=0){bs.splice(i,1);if(i<sel)sel--}
 }
 draw();
};
function syncLinked(b,k){
 const bs=L.pages[pi].buttons,i=linkedIdx(b);if(i<0||!['label','entity_id','color'].includes(k))return;
 bs[i][k]=b[k];const d=$('canvas').children[i];
 if(k=='label'||k=='entity_id')d.firstChild.textContent=bs[i].label||bs[i].entity_id;
 if(k=='color'){const bar=d.querySelector('i');if(bar)bar.style.background=b.color}
}
function showProps(){
 const b=sel!=null?L.pages[pi].buttons[sel]:null;$('props').style.display=b?'block':'none';if(!b)return;
 const sl=b.type=='slider',cam=b.type=='camera';$('btnprops').style.display=sl?'none':'block';$('slprops').style.display=sl?'block':'none';
 $('camprops').style.display=cam?'block':'none';if(cam)$('fullbright').checked=!!b.full_brightness;$('actwrap').style.display=cam?'none':'block';$('slwrap').style.display=cam?'none':'block';
 for(const k of FIELDS)$(k).value=b[k]!==undefined?b[k]:(k=='color'?'#4A90E2':'');
 cellInfo(b);
 if(!sl&&!cam)updateSliderBox(b);
}
for(const k of FIELDS)$(k).oninput=()=>{
 const b=L.pages[pi].buttons[sel];
 if((k=='min'||k=='max')){if($(k).value==='')delete b[k];else b[k]=+$(k).value}else b[k]=$(k).value;
 const d=$('canvas').children[sel];
 if(k=='label'||k=='entity_id')d.firstChild.textContent=b.label||b.entity_id;
 if(b.type!='slider'&&b.type!='camera'){syncLinked(b,k);if(k=='entity_id')updateSliderBox(b)}
 if(k=='color'){d.style.background=b.color;const i=d.querySelector('i');if(i)i.style.background=b.color}};
$('add').onclick=()=>addW({type:'button',label:'New',icon:'power',x:20,y:20,w:140,h:80,action:'toggle'});
$('fullbright').onchange=()=>{const b=L.pages[pi].buttons[sel];if(!b)return;if($('fullbright').checked)b.full_brightness=true;else delete b.full_brightness};
$('addcam').onclick=()=>addW({type:'camera',label:'Camera',icon:'video',x:20,y:20,w:140,h:80,color:'#8E6BBF'});
$('addsl').onclick=()=>addW({type:'slider',label:'Brightness',x:20,y:20,w:200,h:60});
function addW(o){const bs=L.pages[pi].buttons;bs.push(Object.assign({id:'b'+Date.now(),entity_id:'',color:'#4A90E2'},o));sel=bs.length-1;draw()}
$('del').onclick=()=>{const bs=L.pages[pi].buttons,b=bs[sel];bs.splice(sel,1);for(let i=bs.length-1;i>=0;i--)if(bs[i].linked===b.id)bs.splice(i,1);sel=null;draw()};
$('addpage').onclick=()=>{L.pages.push({name:'Page '+(L.pages.length+1),buttons:[]});pi=L.pages.length-1;sel=null;draw()};
$('renpage').onclick=()=>{const n=prompt('Page name',L.pages[pi].name);if(n){L.pages[pi].name=n;draw()}};
$('delpage').onclick=()=>{if(L.pages.length>1&&confirm('Delete page?')){L.pages.splice(pi,1);pi=0;sel=null;draw()}};
$('save').onclick=async()=>{const r=await fetch('/api/layout?o='+orient,{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(L)});
 $('msg').textContent=r.ok?'Saved. (Applied now if it is the active orientation.)':'Save failed: '+await r.text()};
</script></body></html>)HTML";

static const char SETTINGS_HTML[] PROGMEM = R"HTML(<!doctype html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Bedside settings</title>
<style>body{font:15px system-ui,sans-serif;background:#16181b;color:#e6e8ea;padding:14px;max-width:420px}
label{display:block;margin:10px 0 3px;color:#9aa}input,select,button{width:100%;box-sizing:border-box;padding:8px;background:#23262a;color:#eee;border:1px solid #444;border-radius:6px}
a{color:#7ab}</style></head><body>
<h3>Settings &nbsp; <small><a href="/">Layout</a> &middot; <a href="/update">Update</a></small></h3>
<h4>Wi-Fi networks</h4>
<div id="wcur"><small>Loading...</small></div>
<table id="wlist" style="width:100%;border-collapse:collapse"></table>
<button id="wscan" type="button">Scan for networks</button>
<div id="wscanres"></div>
<label style="margin-top:14px">Add a network manually (it can be out of range right now)</label>
<input id="wssid" placeholder="Network name (SSID)" autocomplete="off">
<input id="wpass" type="password" placeholder="Password (empty for an open network)" style="margin-top:6px" autocomplete="new-password">
<button id="wadd" type="button" style="margin-top:6px">Save network</button>
<small>The device joins the saved network with the highest priority that is in range. Saved passwords are never shown; use "Password" to replace one.</small>
<hr style="margin:18px 0">
<form id="f">
<h4>Home Assistant, local address (tried first)</h4>
<label>Host</label><input name="ha_host" placeholder="homeassistant.local or 192.168.1.10">
<label>Port</label><input name="ha_port" type="number" value="8123">
<label><input type="checkbox" name="ha_tls" style="width:auto"> Use TLS (wss)</label>
<h4>External address (fallback when local is unreachable, optional)</h4>
<label>Host</label><input name="ha_ext_host" placeholder="myname.duckdns.org or xxxx.ui.nabu.casa">
<label>Port</label><input name="ha_ext_port" type="number" value="443">
<label><input type="checkbox" name="ha_ext_tls" style="width:auto" checked> Use TLS (wss)</label>
<h4>Screen rotation</h4>
<label>Rotate the display (restarts; portrait has its own layout)</label>
<select name="rotation"><option value="0">0 (landscape, default)</option><option value="1">90 clockwise (portrait)</option><option value="2">180 (landscape upside down)</option><option value="3">90 counter-clockwise (portrait)</option></select>
<h4>Screen brightness range (used by the swipe-down slider)</h4>
<label>Minimum % (5 to 90)</label><input name="bright_min" type="number" min="5" max="90" value="10">
<label>Maximum % (at least 10 above minimum, up to 100)</label><input name="bright_max" type="number" min="15" max="100" value="100">
<label>Long-lived access token (leave blank to keep)</label><input name="ha_token" type="password">
<button type="submit" style="margin-top:14px;background:#2d7a3e">Save &amp; restart</button>
</form><div id="msg"></div>
<hr style="margin:18px 0">
<h4>Maintenance</h4>
<button type="button" id="reboot">Reboot device</button>
<small>Restarts the device. Nothing is erased.</small>
<button type="button" id="freset" style="margin-top:14px;background:#a32727">Factory reset...</button>
<small>Erases saved Wi-Fi networks, the Home Assistant token and addresses, the web password and both layouts, then restarts into setup mode (Wi-Fi "Bedside-Setup"). You are asked to confirm twice.</small>
<div id="mmsg"></div>
<script>

const $=id=>document.getElementById(id);
async function api(path,body){const r=await fetch(path,body===undefined?{}:{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)});
 if(!r.ok)throw new Error(await r.text()||('HTTP '+r.status));return r}
async function loadWifi(){
 const j=await (await api('/api/wifi')).json();
 $('wcur').innerHTML=j.connected?('Connected to <b></b> ('+j.ip+')'):'Not connected to Wi-Fi (setup mode, '+j.ip+')';
 if(j.connected)$('wcur').querySelector('b').textContent=j.connected;
 const t=$('wlist');t.innerHTML='';
 j.networks.forEach((n,i)=>{
  const tr=t.insertRow();tr.style.borderBottom='1px solid #333';
  const c=tr.insertCell();c.style.padding='6px 0';c.textContent=(i+1)+'. '+n.ssid+(n.ssid===j.connected?'  (connected)':'');
  const a=tr.insertCell();a.style.whiteSpace='nowrap';a.style.textAlign='right';
  const mk=(txt,fn)=>{const b=document.createElement('button');b.type='button';b.textContent=txt;b.style.cssText='width:auto;margin:2px;padding:4px 8px';b.onclick=async()=>{try{await fn();loadWifi()}catch(e){alert(e.message)}};a.appendChild(b)};
  if(i>0)mk('\u25B2',()=>api('/api/wifi/move',{ssid:n.ssid,dir:-1}));
  if(i<j.networks.length-1)mk('\u25BC',()=>api('/api/wifi/move',{ssid:n.ssid,dir:1}));
  mk('Password',async()=>{const p=prompt('New password for '+n.ssid+' (8 to 63 characters, empty = open network)');if(p===null)return;
   if(!p&&!confirm('Save '+n.ssid+' as an open network (no password)?'))return;
   await api('/api/wifi/save',{ssid:n.ssid,pass:p})});
  mk('Use now',async()=>{if(!confirm('Switch to '+n.ssid+' and restart the device?'))return;await api('/api/wifi/use',{ssid:n.ssid});$('wcur').textContent='Restarting...'});
  mk('Delete',async()=>{if(confirm('Forget '+n.ssid+'?'))await api('/api/wifi/delete',{ssid:n.ssid})});
 });
}
$('wscan').onclick=async()=>{
 $('wscanres').textContent='Scanning (takes a few seconds)...';
 try{const l=await (await api('/api/wifi/scan')).json();const box=$('wscanres');box.innerHTML='';
  if(!l.length)box.textContent='No networks found.';
  l.forEach(n=>{const d=document.createElement('div');d.style.cssText='display:flex;justify-content:space-between;align-items:center;padding:4px 0';
   const s=document.createElement('span');s.textContent=n.ssid+'  '+n.rssi+' dBm'+(n.open?'':'  (secured)')+(n.saved?'  (saved)':'');d.appendChild(s);
   const b=document.createElement('button');b.type='button';b.style.cssText='width:auto;margin:0;padding:4px 10px';b.textContent=n.saved?'Update':'Add';
   b.onclick=async()=>{let p='';if(!n.open){p=prompt('Password for '+n.ssid);if(p===null)return}
    try{await api('/api/wifi/save',{ssid:n.ssid,pass:p});loadWifi();b.textContent='Saved'}catch(e){alert(e.message)}};
   d.appendChild(b);box.appendChild(d)})}catch(e){$('wscanres').textContent='Scan failed: '+e.message}};
$('wadd').onclick=async()=>{try{await api('/api/wifi/save',{ssid:$('wssid').value.trim(),pass:$('wpass').value});$('wssid').value='';$('wpass').value='';loadWifi()}catch(e){alert(e.message)}};
loadWifi();
$('reboot').onclick=async()=>{if(!confirm('Reboot the device now?'))return;
 try{await api('/api/reboot',{})}catch(e){}$('mmsg').textContent='Rebooting... reload this page in about 20 seconds.'};
$('freset').onclick=async()=>{
 if(!confirm('FACTORY RESET\n\nThis erases ALL saved settings: Wi-Fi networks, Home Assistant token and addresses, the web password and every layout.\n\nContinue?'))return;
 const w=prompt('Second confirmation: type RESET (capital letters) to erase everything.');
 if(w!=='RESET'){$('mmsg').textContent='Factory reset cancelled.';return}
 try{await api('/api/factory-reset',{})}catch(e){}
 $('mmsg').textContent='Erased. The device is restarting into setup mode: join Wi-Fi "Bedside-Setup" and open http://192.168.4.1/settings. The new web password is on the device screen under the gear, About.'};
const f=document.getElementById('f');
fetch('/api/config').then(r=>r.json()).then(j=>{for(const k in j){const e=f.elements[k];if(!e)continue;if(e.type=='checkbox')e.checked=j[k];else e.value=j[k]}});
f.onsubmit=async e=>{e.preventDefault();const o={};for(const el of f.elements){if(!el.name)continue;o[el.name]=el.type=='checkbox'?el.checked:(el.type=='number'||el.name=='rotation')?+el.value:el.value}
 const r=await fetch('/api/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(o)});
 document.getElementById('msg').textContent=r.ok?'Saved'+(o.rotation!==undefined?' (screen rotation '+['0','90 clockwise','180','90 counter-clockwise'][o.rotation]+')':'')+'. Device is restarting, the screen changes when it is back...':'Save failed'};
</script></body></html>)HTML";

static const char UPDATE_HTML[] PROGMEM = R"HTML(<!doctype html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Bedside update</title>
<style>body{font:15px system-ui,sans-serif;background:#16181b;color:#e6e8ea;padding:14px;max-width:420px}
input,button{width:100%;box-sizing:border-box;padding:8px;margin-top:10px;background:#23262a;color:#eee;border:1px solid #444;border-radius:6px}a{color:#7ab}</style></head><body>
<h3>Firmware update &nbsp; <small><a href="/">Layout</a></small></h3>
<p>Running: <b id="ver">...</b></p>
<h4 style="margin-bottom:4px">Update from GitHub</h4>
<button id="chk" style="background:#2b4a7a">Check for a new version</button>
<div id="chkres" style="margin-top:6px"></div>
<button id="inst" style="display:none;background:#2d7a3e">Install</button>
<hr style="margin:18px 0;border-color:#333">
<h4 style="margin-bottom:4px">Or upload a file</h4>
<p>Upload the <code>*-ota.bin</code> file from a GitHub release (not the <code>-full.bin</code>). The device shows the same progress on its screen and reboots when done.</p>
<input type="file" id="f" accept=".bin"><button id="go" style="background:#2d7a3e">Upload &amp; install</button>
<progress id="p" value="0" max="100" style="width:100%;margin-top:10px"></progress>
<div id="stage" style="font-weight:600;margin-top:4px"></div><div id="msg"></div>
<script>
const $=id=>document.getElementById(id);
fetch('/api/version').then(r=>r.text()).then(v=>{$('ver').textContent='v'+v;window.oldVer=v});
const stage=(t,cls)=>{$('stage').textContent=t;$('stage').style.color=cls=='bad'?'#ff5252':cls=='ok'?'#2ed39a':'#e6e8ea'};
function waitForReboot(){
 stage('Rebooting... waiting for the device to come back');$('p').removeAttribute('value');
 let tries=0,wentDown=false;
 const t=setInterval(async()=>{tries++;
  try{const r=await fetch('/api/version',{cache:'no-store'});
   if(r.ok){const v=await r.text();if(wentDown||tries>6){clearInterval(t);$('p').value=100;stage('Done. Device is back on v'+v+(window.oldVer&&v!=window.oldVer?' (updated from v'+window.oldVer+')':''),'ok');$('ver').textContent='v'+v}}
  }catch(e){wentDown=true}
  if(tries>60){clearInterval(t);stage('The device has not come back after 2 minutes. Check its screen.','bad')}},2000);
}
$('chk').onclick=async()=>{$('chkres').textContent='Checking GitHub...';$('inst').style.display='none';
 try{const j=await (await fetch('/api/ota/check')).json();
  if(!j.latest)$('chkres').textContent='Could not reach GitHub (the device needs internet, and the repository must be public).';
  else if(j.newer){$('chkres').textContent='New version '+j.latest+' available (you have v'+j.current+').';$('inst').textContent='Install '+j.latest;$('inst').style.display='block'}
  else $('chkres').textContent='Up to date (latest is '+j.latest+', you have v'+j.current+').';
 }catch(e){$('chkres').textContent='Check failed: '+e}};
$('inst').onclick=async()=>{if(!confirm('Download and install the latest release now? The device reboots when it is done.'))return;
 $('inst').disabled=true;$('chk').disabled=true;$('go').disabled=true;stage('Starting...');
 try{await fetch('/api/ota/install',{method:'POST'})}catch(e){}
 let fails=0;
 const t=setInterval(async()=>{
  try{const s=await (await fetch('/api/ota/status',{cache:'no-store'})).json();fails=0;
   if(s.error){clearInterval(t);stage('Install failed','bad');$('msg').textContent=s.error;$('inst').disabled=false;$('chk').disabled=false;$('go').disabled=false;return}
   if(s.pct>=0)$('p').value=s.pct;else $('p').removeAttribute('value');
   stage(s.phase+(s.pct>=0?' '+s.pct+'%':''));
   if(s.phase=='Rebooting'){clearInterval(t);waitForReboot()}
  }catch(e){fails++;if(fails>=3){clearInterval(t);waitForReboot()}}},700);
};
$('go').onclick=()=>{const f=$('f').files[0];if(!f)return;$('go').disabled=true;$('msg').textContent='';
 const x=new XMLHttpRequest(),fd=new FormData();fd.append('firmware',f);
 stage('Uploading 0%');
 x.upload.onprogress=e=>{if(!e.lengthComputable)return;const p=e.loaded/e.total*100;$('p').value=p;stage('Uploading and writing '+Math.round(p)+'%')};
 x.upload.onload=()=>stage('Verifying the image...');
 x.onload=()=>{if(x.status==200){stage('Installed. Rebooting...','ok');waitForReboot()}else{$('go').disabled=false;stage('Install failed','bad');$('msg').textContent=x.responseText}};
 x.onerror=()=>{stage('Connection lost during upload. The device may be rebooting; checking...','bad');waitForReboot()};
 x.open('POST','/update');x.send(fd)};
</script></body></html>)HTML";
