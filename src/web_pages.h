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
#canvas{position:relative;width:720px;height:420px;background:#101214;border:1px solid #444;touch-action:none}
.b{position:absolute;box-sizing:border-box;border-radius:12px;display:flex;align-items:center;justify-content:center;
 text-align:center;color:#fff;cursor:move;user-select:none;font-size:15px;overflow:hidden}
.b.sel{outline:2px solid #fff}
.h{position:absolute;right:0;bottom:0;width:18px;height:18px;background:#fff8;cursor:nwse-resize}
#form{width:260px}
label{display:block;margin:6px 0 2px;color:#9aa}
input,select,button{width:100%;box-sizing:border-box;padding:6px;background:#23262a;color:#eee;border:1px solid #444;border-radius:6px}
button{cursor:pointer;margin-top:6px}
.tab{display:inline-block;width:auto;padding:6px 10px;margin-right:4px}
.tab.on{background:#3d7bd9}
</style></head><body>
<h3>Bedside layout editor &nbsp; <small><a href="/settings">Settings</a></small></h3>
<div id="tabs"></div>
<div id="wrap">
<div id="canvas"></div>
<div id="form">
 <button id="add">+ Add button</button>
 <div id="props" style="display:none">
  <label>Label</label><input id="label">
  <label>Icon</label><select id="icon"></select>
  <label>Entity id (e.g. light.bedside)</label><input id="entity_id">
  <label>Action</label><select id="action"><option>toggle</option><option>on</option><option>off</option><option>scene</option><option>script</option><option>press</option></select>
  <label>Color</label><input id="color" type="color">
  <button id="del">Delete button</button>
 </div>
 <hr><button id="addpage">+ Add page</button><button id="renpage">Rename page</button><button id="delpage">Delete page</button>
 <button id="save" style="background:#2d7a3e">Save &amp; apply</button>
 <div id="msg"></div>
</div></div>
<script>
const S=1.5,W=480,H=280;
const ICONS=['','power','eye','home','play','pause','bell','settings','ok','wifi','audio','charge','image'];
let L={pages:[]},pi=0,sel=null;
const $=id=>document.getElementById(id);
ICONS.forEach(i=>$('icon').add(new Option(i||'(none)',i)));
fetch('/api/layout').then(r=>r.json()).then(j=>{L=j;if(!L.pages.length)L.pages=[{name:'Page 1',buttons:[]}];draw()});
function draw(){
 $('tabs').innerHTML='';
 L.pages.forEach((p,i)=>{const t=document.createElement('button');t.className='tab'+(i==pi?' on':'');t.textContent=p.name;t.onclick=()=>{pi=i;sel=null;draw()};$('tabs').appendChild(t)});
 const c=$('canvas');c.innerHTML='';
 const bs=L.pages[pi].buttons;
 bs.forEach((b,i)=>{
  const d=document.createElement('div');d.className='b'+(sel===i?' sel':'');
  d.style.cssText+=`left:${b.x*S}px;top:${b.y*S}px;width:${b.w*S}px;height:${b.h*S}px;background:${b.color||'#4A90E2'}`;
  d.textContent=b.label||b.entity_id||'';
  d.onpointerdown=e=>{sel=i;drag(e,b,'move');showProps();markSel()};
  const h=document.createElement('div');h.className='h';h.onpointerdown=e=>{e.stopPropagation();sel=i;drag(e,b,'size')};
  d.appendChild(h);c.appendChild(d)});
 showProps();
}
function markSel(){[...$('canvas').children].forEach((d,i)=>d.classList.toggle('sel',i===sel))}
function drag(e,b,mode){
 e.preventDefault();const sx=e.clientX,sy=e.clientY,ox=b.x,oy=b.y,ow=b.w,oh=b.h,el=e.currentTarget.classList.contains('h')?e.currentTarget.parentNode:e.currentTarget;
 const mv=ev=>{const dx=Math.round((ev.clientX-sx)/S/5)*5,dy=Math.round((ev.clientY-sy)/S/5)*5;
  if(mode=='move'){b.x=Math.max(0,Math.min(W-b.w,ox+dx));b.y=Math.max(0,Math.min(H-b.h,oy+dy))}
  else{b.w=Math.max(40,Math.min(W-b.x,ow+dx));b.h=Math.max(30,Math.min(H-b.y,oh+dy))}
  el.style.left=b.x*S+'px';el.style.top=b.y*S+'px';el.style.width=b.w*S+'px';el.style.height=b.h*S+'px'};
 const up=()=>{removeEventListener('pointermove',mv);removeEventListener('pointerup',up)};
 addEventListener('pointermove',mv);addEventListener('pointerup',up);
}
function showProps(){
 const b=sel!=null?L.pages[pi].buttons[sel]:null;$('props').style.display=b?'block':'none';if(!b)return;
 for(const k of['label','icon','entity_id','action','color'])$(k).value=b[k]||(k=='color'?'#4A90E2':'');
}
for(const k of['label','icon','entity_id','action','color'])$(k).oninput=()=>{
 const b=L.pages[pi].buttons[sel];b[k]=$(k).value;const d=$('canvas').children[sel];
 if(k=='label'||k=='entity_id')d.firstChild.textContent=b.label||b.entity_id;if(k=='color')d.style.background=b.color};
$('add').onclick=()=>{const bs=L.pages[pi].buttons;bs.push({id:'b'+Date.now(),label:'New',icon:'power',x:10,y:10,w:140,h:80,entity_id:'',action:'toggle',color:'#4A90E2'});sel=bs.length-1;draw()};
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
<h3>Settings &nbsp; <small><a href="/">Layout</a></small></h3>
<form id="f">
<label>Wi-Fi SSID</label><input name="wifi_ssid">
<label>Wi-Fi password (leave blank to keep)</label><input name="wifi_pass" type="password">
<label>Home Assistant host</label><input name="ha_host" placeholder="homeassistant.local">
<label>Port</label><input name="ha_port" type="number" value="8123">
<label><input type="checkbox" name="ha_tls" style="width:auto"> Use TLS (wss)</label>
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
