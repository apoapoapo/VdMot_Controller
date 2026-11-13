/*
 * Status and maintenance page (bring-up UI of firmware v2).
 *
 * SPDX-License-Identifier: GPL-3.0-only
 */
#pragma once

namespace app
{

static const char kIndexHtml[] = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>VdMot Controller</title>
<style>
:root{--bg:#f6f7f9;--fg:#1d2330;--mut:#6b7280;--card:#fff;--line:#e3e6eb;--acc:#2563eb;--bad:#c62828;--ok:#2e7d32;--warn:#b26a00}
@media(prefers-color-scheme:dark){:root{--bg:#14171c;--fg:#e6e8eb;--mut:#9aa3ae;--card:#1d2128;--line:#2c323b;--acc:#6ea0ff;--bad:#ff6b6b;--ok:#6fcf73;--warn:#ffb74d}}
*{box-sizing:border-box}body{margin:0;font:14px/1.4 system-ui,sans-serif;background:var(--bg);color:var(--fg)}
header{padding:12px 16px;border-bottom:1px solid var(--line);display:flex;flex-wrap:wrap;gap:12px;align-items:center}
h1{font-size:18px;margin:0 12px 0 0}h2{font-size:15px;margin:0 0 8px}
main{padding:16px;display:grid;gap:16px;max-width:1200px;margin:0 auto}
section{background:var(--card);border:1px solid var(--line);border-radius:8px;padding:12px;overflow-x:auto}
table{border-collapse:collapse;width:100%}td,th{padding:4px 6px;border-bottom:1px solid var(--line);text-align:left;white-space:nowrap}
th{color:var(--mut);font-weight:500}.num{text-align:right;font-variant-numeric:tabular-nums}
.pill{padding:2px 8px;border-radius:10px;border:1px solid var(--line)}.ok{color:var(--ok)}.bad{color:var(--bad)}.warn{color:var(--warn)}
input[type=number]{width:70px}input,button,select{font:inherit;padding:3px 6px;border:1px solid var(--line);border-radius:5px;background:var(--card);color:var(--fg)}
button{cursor:pointer}button.p{background:var(--acc);color:#fff;border-color:var(--acc)}
.row{display:flex;flex-wrap:wrap;gap:8px;align-items:center;margin:6px 0}.mut{color:var(--mut)}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(220px,1fr));gap:4px 16px}
label{display:flex;justify-content:space-between;gap:8px}progress{width:200px}
.chg input{border-color:var(--warn)}small.def{color:var(--warn);font-size:12px}
#toast{position:fixed;left:50%;bottom:20px;transform:translateX(-50%);background:var(--fg);color:var(--bg);padding:8px 14px;border-radius:6px;display:none;max-width:90vw}
</style></head><body>
<div id="toast"></div>
<header><h1>VdMot Controller</h1>
<span id="link" class="pill">…</span><span id="ver" class="mut"></span><span id="net" class="mut"></span>
<span id="err" class="bad"></span></header>
<main>
<section><h2>Valves</h2>
<div class="row"><button onclick="act('calibrate','all')">Calibrate all</button>
<button onclick="act('detect','all')">Detect all</button><button onclick="act('stop','all')">Stop all</button></div>
<table><thead><tr><th>#</th><th>State</th><th class="num">Actual %</th><th class="num">Target %</th>
<th>Set</th><th class="num">mA</th><th>Flags</th><th class="num">Strokes open/close</th><th class="num">Moves</th><th></th></tr></thead>
<tbody id="valves"></tbody></table></section>
<section><h2>Sensors</h2><div class="row"><button onclick="act('scan')">Search bus</button><span id="scan" class="mut"></span></div>
<table><thead><tr><th>ROM id</th><th>Type</th><th class="num">Value</th><th>Status</th><th class="num">Age s</th></tr></thead>
<tbody id="sensors"></tbody></table></section>
<section><h2>STM32 settings</h2>
<form id="sys" onsubmit="save('system',this);return false"><div class="grid" id="sysf"></div>
<div class="row"><button class="p">Save system settings</button><button type="button" onclick="defaults('sysf','sys')">Load defaults</button>
<span class="mut">Orange: differs from the default. Settings are stored on the STM32 and survive updates.</span></div></form>
<form id="mot" onsubmit="save('motor',this);return false"><div class="grid" id="motf"></div>
<div class="row"><button class="p">Save motor parameters</button><button type="button" onclick="defaults('motf','motor')">Load defaults</button></div></form>
<form onsubmit="save('channels',this);return false"><div class="row">Enabled channels <span id="chans"></span>
<button class="p">Save</button></div></form></section>
<section><h2>Firmware</h2>
<div class="row">STM32 image (.bin) <input type="file" id="fstm" accept=".bin"><button onclick="upload('stm32')">Flash STM32</button></div>
<div class="row">ESP32 image (.bin) <input type="file" id="fesp" accept=".bin"><button onclick="upload('esp')">Update ESP32</button></div>
<div class="row"><progress id="prog" max="100" value="0"></progress><span id="upd" class="mut"></span></div>
<div class="row mut" id="updd" style="font-family:monospace;white-space:normal;word-break:break-all"></div>
<div class="row"><button onclick="act('stm32/reset')">Restart STM32</button><button onclick="act('stm32/hardreset')">Hard reset STM32</button>
<button onclick="if(confirm('Restart the ESP32?'))act('reboot')">Restart ESP32</button></div>
<div class="row"><span id="prev" class="mut"></span><button id="rb" onclick="if(confirm('Start the previous ESP32 firmware (e.g. the legacy 1.x)?'))act('esp/rollback')">Start previous ESP32 firmware</button></div></section>
<section><h2>Diagnostics</h2><div class="grid" id="diag"></div></section>
<section><h2>Log</h2><table><tbody id="log"></tbody></table></section>
</main>
<script>
const MODES=['disabled','unknown','not connected','idle','opening','closing','calibrating','queued','fault'];
const FAULTS=['','open circuit','blocked','timeout','overcurrent','calibration failed','no pulses','count mismatch'];
const KINDS=['','DS18B20 °C','DS2438 VAD V','DS2438 VDD V','DS2438 °C'];
const SSTAT=['ok','CRC error','unreachable','not measured','out of range'];
const RESETS=['unknown','power-on','reset pin','software','watchdog','brown-out','low-power'];
function dur(s){if(s===undefined)return '?';const d=Math.floor(s/86400),h=Math.floor(s%86400/3600),m=Math.floor(s%3600/60);return (d?d+'d ':'')+(d||h?h+'h ':'')+m+'min'}
const SYSF=[['link_timeout_s','Link timeout s (0 = off)'],['failsafe_pos','Failsafe position ‰ (65535 = hold)'],
['startup_pos','Startup position ‰ (65535 = last)'],['sensor_interval_s','Sensor interval s'],
['recal_after_moves','Recalibrate after moves'],['recal_after_days','Recalibrate after days']];
const MOTF=[['endstop_factor_open','Endstop factor open (x/10)'],['endstop_factor_close','Endstop factor close (x/10)'],
['endstop_abs_ma','Endstop absolute mA'],['inrush_blank_ms','Inrush blanking ms'],['opencircuit_ma10','Open circuit 0.1 mA'],
['opencircuit_ms','Open circuit ms'],['move_timeout_s','Move timeout s'],['min_stroke_counts','Min. stroke pulses'],['max_calib_retries','Calibration retries']];
let formsDone=false,editing=new Set();
const $=id=>document.getElementById(id);
const pct=v=>v===65535?'?':(v/10).toFixed(1);
function esc(s){return String(s).replace(/[&<>"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]))}
let toastT=0;function toast(m){const t=$('toast');t.textContent=m;t.style.display='block';clearTimeout(toastT);toastT=setTimeout(()=>t.style.display='none',4000)}
const DONE={'stm32/reset':'Restart sent – the STM32 restarts (see boot # and log)','stm32/hardreset':'Reset pulse sent – only reaches the STM32 with jumper X20',
calibrate:'Calibration scheduled',detect:'Detection scheduled',stop:'Stop sent',scan:'Bus search started',target:'Target sent',
'config/system':'System settings saved','config/motor':'Motor parameters saved','config/channels':'Channels saved',reboot:'ESP32 restarts…','esp/rollback':'Starting the previous firmware…'};
/* actions are sent url-encoded (the ESP32 web server does not handle empty multipart bodies) */
async function post(path,body){let j;const c=new AbortController();const t=setTimeout(()=>c.abort(),8000);
try{const r=await fetch('/api/'+path,{method:'POST',body:new URLSearchParams(body||undefined),signal:c.signal});j=await r.json().catch(()=>({ok:false,error:'HTTP '+r.status}))}
catch(e){j={ok:false,error:c.signal.aborted?'no answer from the ESP32':'ESP32 not reachable'}}finally{clearTimeout(t)}
if(!j.ok)alert(j.error||'failed');else toast(DONE[path]||'Done');refresh();return j}
function act(a,ch){const f=new FormData();if(ch!==undefined)f.append('ch',ch);post(a,f)}
let lastState=null;
function defaults(id,kind){const d=lastState&&lastState.defaults&&lastState.defaults[kind];if(!d)return;
Object.entries(d).forEach(([k,v])=>{const i=$(id).querySelector(`[name=${k}]`);if(i)i.value=v});dirty.add(id);toast('Defaults loaded – click Save to apply')}
let espUpload=false;
function setT(ch){const v=$('t'+ch).value;const f=new FormData();f.append('ch',ch);f.append('pos',v);editing.delete(ch);post('target',f)}
/* forms with unsaved edits are not overwritten by the periodic refresh */
const dirty=new Set();const FORMID={system:'sysf',motor:'motf',channels:'chans'};
['input','change'].forEach(ev=>document.addEventListener(ev,e=>{const f=e.target.closest&&e.target.closest('#sysf,#motf,#chans');if(f)dirty.add(f.id)}));
async function save(kind,form){const j=await post('config/'+kind,new FormData(form));if(j.ok)dirty.delete(FORMID[kind]);return false}
function flags(f){const n=[];if(f&1)n.push('cal');if(f&2)n.push('cal pending');if(f&4)n.push('pos?');if(f&8)n.push('end');if(f&16)n.push('failsafe');return n.join(', ')}
function upload(kind){const fi=$(kind==='esp'?'fesp':'fstm').files[0];if(!fi){alert('choose a file');return}
const x=new XMLHttpRequest();const f=new FormData();f.append('file',fi,fi.name);
espUpload=kind==='esp';x.upload.onprogress=e=>{$('prog').value=e.loaded*100/e.total;$('upd').textContent='uploading '+kind};
x.onload=()=>{let j={};try{j=JSON.parse(x.responseText)}catch(e){}$('upd').textContent=j.ok?(kind==='esp'?'ESP32 updated, restarting…':'flashing STM32…'):('error: '+(j.error||x.status))};
x.open('POST','/api/'+kind+'/firmware');x.send(f)}
function fill(id,fields,vals,defs){const g=$(id);if(!formsDone)g.innerHTML=fields.map(([k,l])=>`<label data-k=${k}><span>${l} <small class=def></small></span><input type=number name=${k}></label>`).join('');
if(!vals)return;fields.forEach(([k])=>{const lab=g.querySelector(`[data-k=${k}]`);const diff=defs&&vals[k]!==defs[k];
lab.classList.toggle('chg',!!diff);lab.querySelector('small').textContent=diff?`(default ${defs[k]})`:''});
if(!dirty.has(id))fields.forEach(([k])=>{g.querySelector(`[name=${k}]`).value=vals[k]})}
async function refresh(){let s;try{s=await (await fetch('/api/state')).json()}catch(e){$('link').textContent='ESP32 not reachable';return}
const l=s.link;$('link').textContent='STM32 '+l.state;$('link').className='pill '+(l.state==='connected'?'ok':'bad');
$('ver').textContent='ESP32 '+s.esp.version+(s.stm?(' · STM32 '+s.stm.version+' ('+s.stm.mcu+', '+s.stm.hw+') · boot #'+s.stm.boot_count+', last reset: '+(RESETS[s.stm.reset_cause]||s.stm.reset_cause)+', up '+dur(s.diag&&s.diag.stm_uptime_s)):'');
$('net').textContent=s.net.interface+' '+s.net.ip+(s.net.ap?(' · AP '+s.net.ap_ssid):'');$('err').textContent=s.error||'';
/* the valve table is built once; afterwards only the cells are updated, so
 * an input that has the focus is not replaced */
if($('valves').rows.length!==s.valves.length)$('valves').innerHTML=s.valves.map(v=>`<tr id=vr${v.ch}><td>${v.ch+1}</td><td></td><td class=num></td><td class=num></td>
<td><input id=t${v.ch} type=number min=0 max=100 step=0.5 oninput="editing.add(${v.ch})"><button onclick="setT(${v.ch})">Set</button></td>
<td class=num></td><td></td><td class=num></td><td class=num></td><td><button onclick="act('calibrate',${v.ch})">Calibrate</button></td></tr>`).join('');
s.valves.forEach(v=>{const c=$('vr'+v.ch).cells;c[1].className=v.mode===8?'bad':(v.mode===2||v.mode===0?'mut':'');
c[1].textContent=(MODES[v.mode]||v.mode)+(v.fault?' – '+FAULTS[v.fault]:'');c[2].textContent=pct(v.actual);c[3].textContent=pct(v.target);
const i=$('t'+v.ch);if(!editing.has(v.ch)&&document.activeElement!==i)i.value=v.target/10;
c[5].textContent=(v.current/10).toFixed(1);c[6].textContent=flags(v.flags);c[7].textContent=(v.open_counts??'')+'/'+(v.close_counts??'');c[8].textContent=v.moves??''});
$('sensors').innerHTML=s.sensors.map(x=>{const t=x.kind===2||x.kind===3;return `<tr><td>${x.rom}</td><td>${KINDS[x.kind]||x.kind}</td>
<td class=num>${x.age===65535?'':(x.value/1000).toFixed(t?3:2)}</td><td class="${x.status?'warn':''}">${SSTAT[x.status]||x.status}</td><td class=num>${x.age===65535?'':x.age}</td></tr>`}).join('');
$('scan').textContent=s.scan?`${s.scan.total} devices, bus ${s.scan.bus_ok?'ok':'error'}`:'';
lastState=s;const D=s.defaults||{};fill('sysf',SYSF,s.sys,D.sys);fill('motf',MOTF,s.motor,D.motor);
if(!formsDone){$('chans').innerHTML=s.valves.map(v=>`<label style="display:inline-flex;gap:2px"><input type=checkbox name=ch value=${v.ch}>${v.ch+1}</label>`).join(' ');}
if(!dirty.has('chans'))s.valves.forEach(v=>{$('chans').querySelector(`[value="${v.ch}"]`).checked=v.mode!==0});
formsDone=true;
const p=s.esp.previous;$('prev').textContent=p?`Previous ESP32 firmware in the other slot: ${p.project} ${p.version} (${p.date})`:'No previous ESP32 firmware';$('rb').disabled=!p;
const u=s.update;if(!espUpload&&(u.active||u.result)){$('prog').value=u.percent;$('upd').textContent=u.active?`STM32 update: ${u.phase} ${u.percent}%`:`Last STM32 update: ${u.result||u.phase}`;
const ask=u.active&&u.phase.indexOf('RESET')>=0;$('upd').className=ask?'warn':'mut';$('upd').style.fontWeight=ask?'700':'';
if(ask)$('upd').textContent='Press the RESET button on the STM32 (BlackPill) now – waiting up to 30 s';$('updd').textContent=u.detail?('Bootloader entry: '+u.detail):''}
const d=Object.assign({},s.link.stats,s.diag||{});$('diag').innerHTML=Object.entries(d).map(([k,v])=>`<span>${esc(k)}: <b>${esc(v)}</b></span>`).join('');
$('log').innerHTML=s.log.slice().reverse().map(e=>`<tr><td class=mut>${(e.ms/1000).toFixed(0)} s</td><td>${['error','warn','info','debug'][e.level]}</td><td>${esc(e.text)}</td></tr>`).join('')}
refresh();setInterval(refresh,2000);
</script></body></html>)HTML";

} // namespace app
