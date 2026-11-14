/*
 * Web page: overview, room setup, settings and maintenance (firmware v2,
 * phase 2b). The full redesign follows in phase 4.
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
nav{display:flex;gap:4px;padding:8px 16px 0;border-bottom:1px solid var(--line);flex-wrap:wrap}
nav a{padding:6px 12px;border:1px solid transparent;border-bottom:none;border-radius:6px 6px 0 0;color:var(--mut);text-decoration:none}
nav a.on{background:var(--card);border-color:var(--line);color:var(--fg);margin-bottom:-1px}
main{padding:16px;max-width:1300px;margin:0 auto}.tab{display:none;gap:16px}.tab.on{display:grid}
section{background:var(--card);border:1px solid var(--line);border-radius:8px;padding:12px;overflow-x:auto}
table{border-collapse:collapse;width:100%}td,th{padding:4px 6px;border-bottom:1px solid var(--line);text-align:left;white-space:nowrap}
th{color:var(--mut);font-weight:500}.num{text-align:right;font-variant-numeric:tabular-nums}
.pill{padding:2px 8px;border-radius:10px;border:1px solid var(--line)}.ok{color:var(--ok)}.bad{color:var(--bad)}.warn{color:var(--warn)}
input[type=number]{width:70px}input,button,select{font:inherit;padding:3px 6px;border:1px solid var(--line);border-radius:5px;background:var(--card);color:var(--fg)}
input.w{width:220px}input.s{width:110px}input.n{width:170px}
ul.help{margin:0 0 8px;padding-left:18px;color:var(--mut)}ul.help li{margin:2px 0}
.tags{display:flex;flex-wrap:wrap;gap:4px;align-items:center;min-width:160px;max-width:300px}
.tag{border:1px solid var(--line);border-radius:10px;padding:1px 4px 1px 8px;white-space:nowrap}
.tag b{cursor:pointer;padding:0 4px;color:var(--mut)}.tag b:hover{color:var(--bad)}.tags select{padding:1px 4px}
button{cursor:pointer}button.p{background:var(--acc);color:#fff;border-color:var(--acc)}button:disabled,input:disabled{opacity:.45;cursor:default}
.row{display:flex;flex-wrap:wrap;gap:8px;align-items:center;margin:6px 0}.mut{color:var(--mut)}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(260px,1fr));gap:4px 16px}
label{display:flex;justify-content:space-between;align-items:center;gap:8px}progress{width:200px}
.chg input{border-color:var(--warn)}small.def{color:var(--warn);font-size:12px}
#toast{position:fixed;left:50%;bottom:20px;transform:translateX(-50%);background:var(--fg);color:var(--bg);padding:8px 14px;border-radius:6px;display:none;max-width:90vw;z-index:9}
</style></head><body>
<div id="toast"></div>
<header><h1 id="title">VdMot Controller</h1>
<span id="link" class="pill">…</span><span id="ver" class="mut"></span><span id="net" class="mut"></span>
<span id="err" class="bad"></span></header>
<nav id="nav"><a href="#ov">Overview</a><a href="#rooms">Rooms &amp; valves</a><a href="#set">Settings</a><a href="#sys">System</a></nav>
<main>
<div class="tab" id="tab-ov">
<section><h2>Zones</h2><table><thead><tr><th>Zone</th><th class="num">Temperature °C</th><th class="num">Supply °C</th><th>Heat setpoint °C</th><th>Cool setpoint °C</th><th>On</th><th>Mode</th>
<th class="num">Output %</th><th>State</th></tr></thead><tbody id="zones"></tbody></table>
<div class="row mut" id="nozones">No zones yet – create them under “Rooms &amp; valves”.</div>
<div class="row" id="flow"></div></section>
<section><h2>Valves</h2>
<div class="row"><button onclick="act('calibrate','all')">Calibrate all</button>
<button onclick="act('detect','all')">Detect all</button><button onclick="act('stop','all')">Stop all</button></div>
<table><thead><tr><th>#</th><th>Name</th><th>Zone</th><th>State</th><th class="num">Actual %</th><th class="num">Target %</th>
<th class="num" title="requested by controller or command → sent after limits and flow guard">Requested → effective %</th>
<th>Set</th><th class="num">mA</th><th>Flags</th><th class="num">Strokes open/close</th><th class="num">Moves</th><th></th></tr></thead>
<tbody id="valves"></tbody></table></section>
</div>

<div class="tab" id="tab-rooms">
<section><h2>Zones</h2>
<ul class="help"><li>A zone is a room or heating circuit with one controller; the valves are assigned below.</li>
<li>Mode <i>control</i>: the ESP32 controls the valves from the temperature. <i>manual</i>: the valve positions are set from outside (web, Home Assistant).</li>
<li>Control temperature: one or more sources; with several, the mean of the valid ones is used. Further temperatures are only shown with the zone (e.g. return temperatures).</li>
<li>Profiles: floor (Xp 2 K, Ti 3600 s, sample 300 s), radiator (Xp 1.5 K, Ti 900 s, sample 60 s), custom.</li>
<li>Heat / cool setpoint: target temperature while the water delivers heat or cold (see Settings → Heating / cooling). On: off closes the valves to their minimum opening.</li>
<li>Supply temperature: the supply sensor of this zone (e.g. its distributor); empty = the global one.</li>
<li>Fallback: valve position while no control temperature is valid (empty = global setting).</li></ul>
<table><thead><tr><th>Name</th><th>Mode</th><th>Heat °C</th><th>Cool °C</th><th>On</th><th>Control temperature</th><th>Further temperatures</th><th>Profile</th>
<th>Supply temperature</th><th>Xp K</th><th>Ti s</th><th>Sample s</th><th>Fallback %</th><th></th></tr></thead><tbody id="ezones"></tbody></table>
<div class="row"><button onclick="addZone()">Add zone</button><button class="p" onclick="saveZones()">Save zones</button></div></section>
<section><h2>Valves</h2>
<ul class="help"><li>Enabled: channels without a valve can be switched off; the STM32 then neither detects nor moves them.</li>
<li>Without a zone a valve is set directly (overview).</li>
<li>Min/max limit every target; min is the minimum opening, also with heating off.</li>
<li>Factor scales the zone output (simple hydraulic balancing between the circuits of a zone).</li>
<li>Flow priority: valves opened first to reach the minimum total opening (Settings).</li></ul>
<table><thead><tr><th>#</th><th>Enabled</th><th>Name</th><th>Zone</th><th>Min %</th><th>Max %</th><th>Factor %</th><th>Flow priority</th></tr></thead><tbody id="evalves"></tbody></table>
<div class="row"><button class="p" onclick="saveValves()">Save valves</button></div></section>
<section><h2>Temperature sources</h2>
<ul class="help"><li>1-Wire sensors found on the bus are offered below the table.</li>
<li>MQTT topic: the topic the value is published on, e.g. by Home Assistant or zigbee2mqtt. Empty = the default topic shown in grey, to which e.g. a Home Assistant automation can publish.</li>
<li>JSON field: only if the payload is JSON, e.g. <code>{"temperature":21.5,"humidity":40}</code> → <code>temperature</code>; nested fields with a dot (<code>sensor.temp</code>). Empty = the payload is a plain number.</li>
<li>Offset: correction added to the value.</li>
<li>Max age: after this time without a new value the source counts as stale; zones using it go to their fallback position (0 = never). Publishers that only send on change need a regular resend.</li>
<li>MQTT values are received once the MQTT integration is in place (next firmware step).</li></ul>
<table><thead><tr><th>Name</th><th>Type</th><th>ROM id / MQTT topic</th><th>JSON field</th><th>Offset K</th><th>Max age s</th><th class="num">Value °C</th><th>State</th><th>Used by</th><th></th></tr></thead><tbody id="esources"></tbody></table>
<div class="row"><button onclick="addSource('mqtt')">Add MQTT source</button><button class="p" onclick="saveSources()">Save sources</button></div>
<div class="row mut" id="unused"></div></section>
</div>

<div class="tab" id="tab-set">
<section><h2>Heating / cooling</h2><div class="grid" id="f-hvac"></div>
<ul class="help"><li>Each zone compares its supply temperature with its room temperature: at least <i>margin</i> above = heat is supplied (heat setpoint), at least <i>margin</i> below = cold is supplied (cool setpoint), in between = nothing is supplied and the controller pauses (valves keep their position, no wind-up). A change must last 5 minutes; once heating or cooling, half the margin is enough to continue.</li>
<li>Mode auto: from the supply temperature. heat / cool: only this one; the other medium pauses. Without a supply sensor: auto and heat heat, cool cools, never paused.</li></ul>
<div class="row"><button class="p" onclick="saveForm('hvac')">Save</button></div></section>
<section><h2>Flow guard and fallback</h2><div class="grid" id="f-flow"></div>
<ul class="help"><li>Fallback position: used while a zone has no valid temperature; also written to the STM32 as its failsafe position (ESP32 gone).</li>
<li>Minimum total opening: minimum sum of all valve positions in percentage points (150 = one and a half open valves, 0 = off). Flow priority valves are opened first; valves with a fault do not count.</li></ul>
<div class="row"><button class="p" onclick="saveForm('flow')">Save</button></div></section>
<section><h2>Device</h2><div class="grid" id="f-device"></div>
<div class="row"><button class="p" onclick="saveForm('device')">Save</button><span class="mut">The name is also the host name. Leave a password empty to keep it.</span></div></section>
<section><h2>Network</h2><div class="grid" id="f-network"></div>
<div class="row"><button class="p" onclick="saveForm('network')">Save</button><span class="mut">Takes effect after a restart. If no network comes up, the ESP32 opens the access point VdMot-XXXX (password vdmot-setup).</span></div></section>
<section><h2>MQTT</h2><div class="grid" id="f-mqtt"></div>
<div class="row"><button class="p" onclick="saveForm('mqtt')">Save</button><span class="mut">Stored already; the MQTT / Home Assistant integration follows in the next firmware step.</span></div></section>
<section><h2>STM32 settings</h2>
<form id="sys" onsubmit="save('system',this);return false"><div class="grid" id="sysf"></div>
<div class="row"><button class="p">Save system settings</button><button type="button" onclick="defaults('sysf','sys')">Load defaults</button>
<span class="mut">Orange: differs from the default. Settings are stored on the STM32 and survive updates. The failsafe position follows the fallback position.</span></div></form>
<form id="mot" onsubmit="save('motor',this);return false"><div class="grid" id="motf"></div>
<div class="row"><button class="p">Save motor parameters</button><button type="button" onclick="defaults('motf','motor')">Load defaults</button></div></form>
</section>
<section><h2>Configuration backup</h2>
<div class="row"><a href="/api/config/export"><button>Export configuration</button></a>
<input type="file" id="fimp" accept=".json"><button onclick="importCfg()">Import and restart</button></div>
<div class="row mut">The export contains the passwords. <span id="cfginfo"></span></div></section>
</div>

<div class="tab" id="tab-sys">
<section><h2>Firmware</h2>
<div class="row">STM32 image (.bin) <input type="file" id="fstm" accept=".bin"><button onclick="upload('stm32')">Flash STM32</button></div>
<div class="row">ESP32 image (.bin) <input type="file" id="fesp" accept=".bin"><button onclick="upload('esp')">Update ESP32</button></div>
<div class="row"><progress id="prog" max="100" value="0"></progress><span id="upd" class="mut"></span></div>
<div class="row mut" id="updd" style="font-family:monospace;white-space:normal;word-break:break-all"></div>
<div class="row"><button onclick="act('stm32/reset')">Restart STM32</button><button onclick="act('stm32/hardreset')">Hard reset STM32</button>
<button onclick="if(confirm('Restart the ESP32?'))act('reboot')">Restart ESP32</button></div>
<div class="row"><span id="prev" class="mut"></span><button id="rb" onclick="if(confirm('Start the previous ESP32 firmware (e.g. the legacy 1.x)?'))act('esp/rollback')">Start previous ESP32 firmware</button></div></section>
<section><h2>1-Wire bus</h2><div class="row"><button onclick="act('scan')">Search bus</button><span id="scan" class="mut"></span></div>
<table><thead><tr><th>ROM id</th><th>Type</th><th class="num">Value</th><th>Status</th><th class="num">Age s</th><th>Source</th></tr></thead>
<tbody id="sensors"></tbody></table></section>
<section><h2>Diagnostics</h2>
<div class="row">ESP32 memory used <progress id="heapbar" max="100" value="0"></progress><span id="heaptxt" class="mut"></span></div>
<div class="grid" id="diag"></div>
<div class="row">Failsafe test: pause the link for <input type=number id=pause min=0 max=3600 value=360> s
<button onclick="pauseLink()">Pause link</button><button onclick="pauseLink(0)">Resume</button>
<span class="mut">The STM32 enters the failsafe after its link timeout (default 300 s; can be lowered in the STM32 settings).</span></div></section>
<section><h2>Log</h2><table><tbody id="log"></tbody></table></section>
</div>
</main>
<script>
const MODES=['disabled','unknown','not connected','idle','opening','closing','calibrating','queued','fault'];
const FAULTS=['','open circuit','blocked','timeout','overcurrent','calibration failed','no pulses','count mismatch'];
const KINDS=['','DS18B20 °C','DS2438 VAD V','DS2438 VDD V','DS2438 °C'];
const SSTAT=['ok','CRC error','unreachable','not measured','out of range'];
const RESETS=['unknown','power-on','reset pin','software','watchdog','brown-out','low-power'];
const ACTIONS={off:'off',idle:'idle',heating:'heating',cooling:'cooling',paused:'paused (no heat/cold supplied)',manual:'manual',fallback:'fallback (no temperature)',hold:'waiting for values'};
function dur(s){if(s===undefined)return '?';const d=Math.floor(s/86400),h=Math.floor(s%86400/3600),m=Math.floor(s%3600/60);return (d?d+'d ':'')+(d||h?h+'h ':'')+m+'min'}
const SYSF=[['link_timeout_s','Link timeout s (0 = off)'],['failsafe_pos','Failsafe position ‰ (= fallback)'],
['startup_pos','Startup position ‰ (65535 = last)'],['sensor_interval_s','Sensor interval s'],
['recal_after_moves','Recalibrate after moves'],['recal_after_days','Recalibrate after days']];
const MOTF=[['endstop_factor_open','Endstop factor open (x/10)'],['endstop_factor_close','Endstop factor close (x/10)'],
['endstop_abs_ma','Endstop absolute mA'],['inrush_blank_ms','Inrush blanking ms'],['opencircuit_ma10','Open circuit 0.1 mA'],
['opencircuit_ms','Open circuit ms'],['move_timeout_s','Move timeout s'],['min_stroke_counts','Min. stroke pulses'],['max_calib_retries','Calibration retries']];
const FORMS={hvac:[['mode','Mode','sel:auto|heat|cool'],['supply_source','Supply temperature (global)','src'],['supply_margin_k','Margin K','num']],
flow:[['fallback_pct','Fallback position %','num'],['min_total_pct','Minimum total opening % points (0 = off)','num']],
device:[['name','Name','text'],['web_user','Web user (empty = no login)','text'],['web_pwd','Web password','pwd'],['tz','Time zone (POSIX)','text'],['ntp','NTP server','text']],
network:[['mode','Interface','sel:auto|ethernet|wifi'],['dhcp','DHCP','bool'],['ip','IP address','text'],['mask','Subnet mask','text'],['gateway','Gateway','text'],['dns','DNS server','text'],['ssid','WiFi SSID','text'],['wifi_pwd','WiFi password','pwd']],
mqtt:[['enabled','Enabled','bool'],['host','Broker host','text'],['port','Port','num'],['user','User','text'],['password','Password','pwd'],['client_id','Client id (empty = from MAC)','text'],['base_topic','Base topic (empty = vdmot/<client id>)','text'],['discovery','Home Assistant discovery','bool'],['discovery_prefix','Discovery prefix','text']]};
let formsDone=false,editing=new Set(),CFG=null,lastState=null;
const $=id=>document.getElementById(id);
const pct=v=>v===65535||v===undefined?'?':(v/10).toFixed(1);
function esc(s){return String(s??'').replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]))}
let toastT=0;function toast(m){const t=$('toast');t.textContent=m;t.style.display='block';clearTimeout(toastT);toastT=setTimeout(()=>t.style.display='none',4000)}
/* tabs */
function tab(){const h=(location.hash||'#ov').slice(1);document.querySelectorAll('.tab').forEach(t=>t.classList.toggle('on',t.id==='tab-'+h));
document.querySelectorAll('#nav a').forEach(a=>a.classList.toggle('on',a.getAttribute('href')==='#'+h))}
window.addEventListener('hashchange',tab);tab();
const DONE={'stm32/reset':'Restart sent – the STM32 restarts (see boot # and log)','stm32/hardreset':'Reset pulse sent – only reaches the STM32 with jumper X20',
calibrate:'Calibration scheduled',detect:'Detection scheduled',stop:'Stop sent',scan:'Bus search started',target:'Target sent',zone:'Zone changed',
'config/system':'System settings saved','config/motor':'Motor parameters saved','config/channels':'Channels saved',reboot:'ESP32 restarts…','link/pause':'Link pause changed','esp/rollback':'Starting the previous firmware…'};
async function req(path,opt){let j;const c=new AbortController();const t=setTimeout(()=>c.abort(),8000);
try{const r=await fetch('/api/'+path,Object.assign({method:'POST',signal:c.signal},opt));j=await r.json().catch(()=>({ok:false,error:'HTTP '+r.status}))}
catch(e){j={ok:false,error:c.signal.aborted?'no answer from the ESP32':'ESP32 not reachable'}}finally{clearTimeout(t)}return j}
/* actions are sent url-encoded (the ESP32 web server does not handle empty multipart bodies) */
async function post(path,body){const j=await req(path,{body:new URLSearchParams(body||undefined)});
if(!j.ok)alert(j.error||'failed');else toast(DONE[path]||'Done');refresh();return j}
function act(a,ch){const f=new FormData();if(ch!==undefined)f.append('ch',ch);post(a,f)}
function defaults(id,kind){const d=lastState&&lastState.defaults&&lastState.defaults[kind];if(!d)return;
Object.entries(d).forEach(([k,v])=>{const i=$(id).querySelector(`[name=${k}]`);if(i)i.value=v});dirty.add(id);toast('Defaults loaded – click Save to apply')}
let espUpload=false;
function pauseLink(sec){const f=new FormData();f.append('seconds',sec===undefined?$('pause').value:sec);post('link/pause',f)}
function setT(ch){const v=$('t'+ch).value;const f=new FormData();f.append('ch',ch);f.append('pos',v);editing.delete('t'+ch);post('target',f)}
function zoneCmd(id,k,v){const f=new FormData();f.append('id',id);f.append(k,v);editing.delete('sp'+id);editing.delete('cp'+id);post('zone',f)}
/* forms with unsaved edits are not overwritten by the periodic refresh */
const dirty=new Set();const FORMID={system:'sysf',motor:'motf'};
['input','change'].forEach(ev=>document.addEventListener(ev,e=>{const f=e.target.closest&&e.target.closest('#sysf,#motf');if(f)dirty.add(f.id)}));
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

/* ---------------- configuration editors ---------------- */
const opt=(v,l,sel)=>`<option value="${esc(v)}"${sel?' selected':''}>${esc(l)}</option>`;
async function loadCfg(){const r=await fetch('/api/config').catch(()=>null);if(!r||!r.ok)return;CFG=await r.json();
ED.zones=CFG.zones.map(z=>Object.assign({},z));ED.sources=CFG.sources.map(s=>Object.assign({},s));
renderZones();renderValves();renderSources();Object.keys(FORMS).forEach(renderForm);$('title').textContent=CFG.device.name}
async function saveSection(sec,data,msg){const j=await req('config?section='+sec,{headers:{'Content-Type':'application/json'},body:JSON.stringify(data)});
if(!j.ok){alert(j.error||'failed');return false}toast(msg);await loadCfg();refresh();
if(j.restart&&confirm('Restart the ESP32 now to apply the change?'))post('reboot');return true}
const ED={zones:[],sources:[]};
const srcLabel=s=>`${s.name} (${s.type==='mqtt'?'MQTT':'1-Wire'})`;
/* source lists as tags: chosen sources with ×, plus a drop-down to add one */
function tagsHtml(ids,max){const src=ED.sources.filter(s=>s.id);
const chips=ids.map(id=>{const s=src.find(x=>x.id===id);return `<span class=tag>${esc(s?srcLabel(s):id)}<b title=remove onclick="tagDel(this,'${id}')">×</b></span>`}).join('');
const free=src.filter(s=>!ids.includes(s.id));
return chips+(ids.length<max&&free.length?`<select onchange="tagAdd(this)">${opt('','+ add',true)}${free.map(s=>opt(s.id,srcLabel(s))).join('')}</select>`:'')+(ids.length?'':'<span class=mut>none</span>')}
function tagSet(el,ids){el.dataset.v=JSON.stringify(ids);el.innerHTML=tagsHtml(ids,+el.dataset.max)}
function tagAdd(sel){const el=sel.closest('.tags');const ids=JSON.parse(el.dataset.v);if(sel.value)ids.push(sel.value);tagSet(el,ids)}
function tagDel(b,id){const el=b.closest('.tags');tagSet(el,JSON.parse(el.dataset.v).filter(x=>x!==id))}
const tags=(k,ids,max)=>`<div class=tags data-k=${k} data-max=${max} data-v='${esc(JSON.stringify(ids||[]))}'>${tagsHtml(ids||[],max)}</div>`;
function usedBy(id){const u=[];ED.zones.forEach(z=>{if((z.control_sources||[]).includes(id))u.push(z.name);if((z.info_sources||[]).includes(id))u.push(z.name+' (info)')});return u.join(', ')}
const PROF={floor:{xp:2,ti_s:3600,sample_s:300},radiator:{xp:1.5,ti_s:900,sample_s:60}};
function renderZones(){$('ezones').innerHTML=ED.zones.map((z,i)=>{const c=z.profile==='custom'?'':' disabled';const q=PROF[z.profile]||z;return `<tr data-i=${i}>
<td><input class=n data-k=name value="${esc(z.name)}"></td>
<td><select data-k=mode>${opt('control','control',z.mode==='control')}${opt('manual','manual',z.mode==='manual')}</select></td>
<td><input type=number data-k=setpoint min=5 max=30 step=0.5 value=${z.setpoint}></td>
<td><input type=number data-k=cool_setpoint min=15 max=35 step=0.5 value=${z.cool_setpoint}></td>
<td><input type=checkbox data-k=enabled${z.enabled?' checked':''}></td>
<td>${tags('control_sources',z.control_sources,4)}</td><td>${tags('info_sources',z.info_sources,4)}</td>
<td><select data-k=profile onchange="collectZones();renderZones()">${['floor','radiator','custom'].map(p=>opt(p,p,z.profile===p)).join('')}</select></td>
<td><select data-k=supply_source>${opt('','– global –',!z.supply_source)}${ED.sources.filter(s=>s.id).map(s=>opt(s.id,srcLabel(s),s.id===z.supply_source)).join('')}</select></td>
<td><input type=number data-k=xp min=0.2 max=20 step=0.1 value=${q.xp}${c}></td><td><input type=number data-k=ti_s min=0 value=${q.ti_s}${c}></td>
<td><input type=number data-k=sample_s min=10 value=${q.sample_s}${c}></td>
<td><input type=number data-k=fallback_pct min=0 max=100 value="${z.fallback_pct<0?'':z.fallback_pct}" placeholder=global></td>
<td><button onclick="collectZones();ED.zones.splice(${i},1);renderZones()">Delete</button></td></tr>`}).join('')||'<tr><td colspan=14 class=mut>No zones</td></tr>'}
function rowVals(tr){const o={};tr.querySelectorAll('[data-k]').forEach(e=>{const k=e.dataset.k;
if(e.classList.contains('tags'))o[k]=JSON.parse(e.dataset.v);else if(e.type==='checkbox')o[k]=e.checked;
else if(e.type==='number')o[k]=e.value===''?null:Number(e.value);else o[k]=e.value});return o}
function collectZones(){document.querySelectorAll('#ezones tr[data-i]').forEach(tr=>{const z=ED.zones[tr.dataset.i];Object.assign(z,rowVals(tr));if(z.fallback_pct===null)z.fallback_pct=-1})}
function addZone(){collectZones();ED.zones.push({name:'Zone '+(ED.zones.length+1),mode:'control',setpoint:20,cool_setpoint:24,enabled:true,supply_source:'',control_sources:[],info_sources:[],profile:'radiator',xp:1.5,ti_s:900,sample_s:60,fallback_pct:-1});renderZones()}
function saveZones(){collectZones();saveSection('zones',ED.zones,'Zones saved')}
function renderValves(){$('evalves').innerHTML=CFG.valves.map((v,i)=>`<tr data-i=${i}><td>${i+1}</td><td><input type=checkbox class=en id=en${i}></td>
<td><input class=n data-k=name value="${esc(v.name)}"></td>
<td><select data-k=zone>${opt('','– none –',!v.zone)}${CFG.zones.map(z=>opt(z.id,z.name,z.id===v.zone)).join('')}</select></td>
<td><input type=number data-k=min_pct min=0 max=100 value=${v.min_pct}></td><td><input type=number data-k=max_pct min=0 max=100 value=${v.max_pct}></td>
<td><input type=number data-k=factor_pct min=10 max=250 value=${v.factor_pct}></td><td><input type=checkbox data-k=flow_priority${v.flow_priority?' checked':''}></td></tr>`).join('');
enabledDone=false;renderEnabled(lastState)}
/* the enabled boxes are filled once per rendering; after a change the
 * wanted state is shown until the STM32 reports it */
let enabledDone=false,enWant=null;
function renderEnabled(s){if(enWant&&s.valves.every((v,i)=>(v.mode!==0)===enWant[i]))enWant=null;
if(enabledDone||!$('en0')||!s)return;enabledDone=true;s.valves.forEach((v,i)=>{const e=$('en'+i);if(e)e.checked=enWant?enWant[i]:v.mode!==0})}
async function saveValves(){const rows=[...document.querySelectorAll('#evalves tr[data-i]')];
if(lastState&&enabledDone){const now=lastState.valves.map(v=>v.mode!==0),want=rows.map((r,i)=>$('en'+i).checked);
if(want.some((w,i)=>w!==now[i])){const f=new FormData();want.forEach((w,i)=>{if(w)f.append('ch',i)});const j=await post('config/channels',f);if(!j.ok)return;enWant=want}}
saveSection('valves',rows.map(rowVals),'Valves saved')}
function renderSources(){$('esources').innerHTML=ED.sources.map((s,i)=>{const m=s.type==='mqtt';return `<tr data-i=${i}>
<td><input class=n data-k=name value="${esc(s.name)}"></td><td>${m?'MQTT':'1-Wire'}</td>
<td>${m?`<input class=w data-k=topic value="${esc(s.topic)}" placeholder="${esc((CFG.mqtt_base||'vdmot/…')+'/source/'+(s.id||'<id>')+'/set')}">`:esc(s.rom)}</td>
<td>${m?`<input class=s data-k=field value="${esc(s.field)}" placeholder="plain number">`:''}</td>
<td><input type=number data-k=offset min=-10 max=10 step=0.1 value=${s.offset}></td>
<td><input type=number data-k=max_age_s min=0 value=${s.max_age_s}></td>
<td class=num id="sv_${esc(s.id)}"></td><td id="ss_${esc(s.id)}" class=mut>${s.id?'':'not saved'}</td><td class=mut>${esc(s.id?usedBy(s.id):'')}</td>
<td><button onclick="collectSources();ED.sources.splice(${i},1);renderSources()">Delete</button></td></tr>`}).join('')||'<tr><td colspan=10 class=mut>No sources</td></tr>';renderUnused()}
function collectSources(){document.querySelectorAll('#esources tr[data-i]').forEach(tr=>Object.assign(ED.sources[tr.dataset.i],rowVals(tr)))}
function addSource(type,rom){collectSources();ED.sources.push(type==='mqtt'?{name:'MQTT '+(ED.sources.length+1),type,topic:'',field:'',offset:0,max_age_s:3600}
:{name:'Sensor '+rom.slice(-5),type,rom,offset:0,max_age_s:300});renderSources()}
function saveSources(){collectSources();saveSection('sources',ED.sources.map(s=>{const o=Object.assign({},s);if(!o.id)delete o.id;return o}),'Sources saved')}
const isTemp=k=>k===1||k===4;
function renderUnused(){if(!lastState||!CFG)return;const used=new Set(ED.sources.filter(s=>s.type==='onewire').map(s=>s.rom));
const un=lastState.sensors.filter(x=>isTemp(x.kind)&&!used.has(x.rom));
$('unused').innerHTML=un.length?'1-Wire sensors not used yet: '+un.map(x=>`<button onclick="addSource('onewire','${x.rom}')">${x.rom} (${(x.value/1000).toFixed(1)} °C) – add</button>`).join(' '):''}
function renderForm(sec){const v=CFG[sec];$('f-'+sec).innerHTML=FORMS[sec].map(([k,l,t])=>{let i;
if(t==='bool')i=`<input type=checkbox data-k=${k}${v[k]?' checked':''}>`;
else if(t==='pwd')i=`<input type=password class=s data-k=${k} placeholder="${v[k+'_set']?'unchanged':'not set'}" autocomplete=new-password>`;
else if(t.startsWith('sel:'))i=`<select data-k=${k}>${t.slice(4).split('|').map(o=>opt(o,o,v[k]===o)).join('')}</select>`;
else if(t==='src')i=`<select data-k=${k}>${opt('','– none –',!v[k])}${CFG.sources.map(s=>opt(s.id,srcLabel(s),s.id===v[k])).join('')}</select>`;
else if(t==='num')i=`<input type=number step=any data-k=${k} value="${esc(v[k])}">`;else i=`<input class=s data-k=${k} value="${esc(v[k])}">`;
return `<label><span>${esc(l)}</span>${i}</label>`}).join('')}
function saveForm(sec){const o=rowVals($('f-'+sec));FORMS[sec].forEach(([k,,t])=>{if(t==='pwd'&&!o[k])delete o[k]});
saveSection(sec,o,'Settings saved')}
async function importCfg(){const fi=$('fimp').files[0];if(!fi){alert('choose a file');return}if(!confirm('Replace the whole configuration and restart?'))return;
const j=await req('config/import',{headers:{'Content-Type':'application/json'},body:await fi.text()});if(!j.ok)alert(j.error||'failed');else toast('Imported – the ESP32 restarts…')}

/* ---------------- periodic refresh ---------------- */
function renderZoneStatus(s){const zs=s.ctl.zones;$('nozones').style.display=zs.length?'none':'';
const key=zs.map(z=>z.id).join();if($('zones').dataset.key!==key){$('zones').dataset.key=key;
$('zones').innerHTML=zs.map(z=>`<tr id="zr${z.id}"><td></td><td class=num></td><td class=num></td>
<td><input id="sp${z.id}" type=number min=5 max=30 step=0.5 oninput="editing.add('sp${z.id}')"><button onclick="zoneCmd('${z.id}','setpoint',$('sp${z.id}').value)">Set</button></td>
<td><input id="cp${z.id}" type=number min=15 max=35 step=0.5 oninput="editing.add('cp${z.id}')"><button onclick="zoneCmd('${z.id}','cool_setpoint',$('cp${z.id}').value)">Set</button></td>
<td><input type=checkbox id="ht${z.id}" onchange="zoneCmd('${z.id}','enabled',this.checked?1:0)"></td>
<td><select id="md${z.id}" onchange="zoneCmd('${z.id}','mode',this.value)">${opt('control','control')}${opt('manual','manual')}</select></td>
<td class=num></td><td></td></tr>`).join('')}
zs.forEach(z=>{const c=$('zr'+z.id).cells;c[0].textContent=z.name;
c[1].textContent=z.temperature===undefined?'–':z.temperature.toFixed(1);c[1].className='num '+(z.source==='ok'?'':'warn');c[1].title='source: '+z.source;
c[2].textContent=z.supply===undefined?'–':z.supply.toFixed(1);
const sp=$('sp'+z.id),cp=$('cp'+z.id);if(!editing.has('sp'+z.id)&&document.activeElement!==sp)sp.value=z.setpoint;
if(!editing.has('cp'+z.id)&&document.activeElement!==cp)cp.value=z.cool_setpoint;
$('ht'+z.id).checked=z.enabled;if(document.activeElement!==$('md'+z.id))$('md'+z.id).value=z.mode;
sp.disabled=cp.disabled=z.mode!=='control';sp.style.fontWeight=z.medium==='heat'?'700':'';cp.style.fontWeight=z.medium==='cool'?'700':'';
c[7].textContent=z.output===undefined?'–':z.output.toFixed(1);
c[8].textContent=z.action==='fallback'&&!z.has_source?'fallback – no control temperature assigned (Rooms & valves)':(ACTIONS[z.action]||z.action);c[8].className=z.action==='fallback'?'warn':(z.action==='heating'||z.action==='cooling'?'ok':'mut')});
const f=s.ctl.flow;$('flow').innerHTML=f.min_total?`Flow guard: total ${pct(f.total)} % of at least ${pct(f.min_total)} %`+
(f.active?` – <b class=warn>active, +${pct(f.added)} % added</b>`:' – not needed')+(f.insufficient?' – <b class=bad>minimum cannot be reached</b>':''):'<span class=mut>Flow guard off (minimum total opening 0)</span>'}
async function refresh(){let s;try{const r=await fetch('/api/state');s=await r.json();if(!r.ok)throw new Error(s.error||'HTTP '+r.status)}
catch(e){$('link').textContent='ESP32 not reachable';$('link').className='pill bad';$('err').textContent=e.message||'';return}
try{render(s)}catch(e){$('err').textContent='page error: '+e.message;console.error(e)}}
function render(s){
const l=s.link;$('link').textContent='STM32 '+l.state+(l.paused_s?` (link paused, ${l.paused_s} s)`:'');$('link').className='pill '+(l.state==='connected'?'ok':'bad');
$('ver').textContent='ESP32 '+s.esp.version+(s.stm?(' · STM32 '+s.stm.version+' ('+s.stm.mcu+', '+s.stm.hw+') · boot #'+s.stm.boot_count+', last reset: '+(RESETS[s.stm.reset_cause]||s.stm.reset_cause)+', up '+dur(s.diag&&s.diag.stm_uptime_s)):'');
$('net').textContent=s.net.interface+' '+s.net.ip+(s.net.ap?(' · AP '+s.net.ap_ssid):'')+(s.net.time?' · '+s.net.time:'');$('err').textContent=s.error||'';
$('cfginfo').textContent='Configuration at start: '+s.config;
renderZoneStatus(s);
const zname=id=>{const z=s.ctl.zones.find(x=>x.id===id);return z?z.name:''};
/* the valve table is built once; afterwards only the cells are updated, so
 * an input that has the focus is not replaced */
if($('valves').rows.length!==s.valves.length)$('valves').innerHTML=s.valves.map(v=>`<tr id=vr${v.ch}><td>${v.ch+1}</td><td></td><td></td><td></td><td class=num></td><td class=num></td><td class=num></td>
<td><input id=t${v.ch} type=number min=0 max=100 step=0.5 oninput="editing.add('t${v.ch}')"><button id=b${v.ch} onclick="setT(${v.ch})">Set</button></td>
<td class=num></td><td></td><td class=num></td><td class=num></td><td><button onclick="act('calibrate',${v.ch})">Calibrate</button></td></tr>`).join('');
s.valves.forEach(v=>{const c=$('vr'+v.ch).cells;const cv=s.ctl.valves[v.ch];c[1].textContent=cv.name;c[2].textContent=zname(cv.zone);
c[3].className=v.mode===8?'bad':(v.mode===2||v.mode===0?'mut':'');
c[3].textContent=(MODES[v.mode]||v.mode)+(v.fault?' – '+FAULTS[v.fault]:'');c[4].textContent=pct(v.actual);c[5].textContent=pct(v.target);
c[6].textContent=cv.effective===undefined?'–':(cv.requested!==cv.effective?pct(cv.requested)+' → ':'')+pct(cv.effective);
const i=$('t'+v.ch);if(!editing.has('t'+v.ch)&&document.activeElement!==i)i.value=(cv.effective??v.target)/10;
i.disabled=$('b'+v.ch).disabled=cv.controlled;i.title=cv.controlled?'set by the zone controller':'';
c[8].textContent=(v.current/10).toFixed(1);c[9].textContent=flags(v.flags);c[10].textContent=(v.open_counts??'')+'/'+(v.close_counts??'');c[11].textContent=v.moves??''});
const srcByRom={};if(CFG)CFG.sources.forEach(x=>{if(x.type==='onewire')srcByRom[x.rom]=x.name});
$('sensors').innerHTML=s.sensors.map(x=>{const t=x.kind===2||x.kind===3;return `<tr><td>${x.rom}</td><td>${KINDS[x.kind]||x.kind}</td>
<td class=num>${x.age===65535?'':(x.value/1000).toFixed(t?3:2)}</td><td class="${x.status?'warn':''}">${SSTAT[x.status]||x.status}</td><td class=num>${x.age===65535?'':x.age}</td>
<td>${isTemp(x.kind)?esc(srcByRom[x.rom]||''):''}</td></tr>`}).join('');
$('scan').textContent=s.scan?`${s.scan.total} devices, bus ${s.scan.bus_ok?'ok':'error'}`:'';
s.ctl.sources.forEach(x=>{const v=$('sv_'+x.id),st=$('ss_'+x.id);if(v)v.textContent=x.value===undefined?'–':x.value.toFixed(2);
if(st){st.textContent=x.state+(x.age!==undefined?` (${x.age} s)`:'');st.className=x.state==='ok'?'ok':'warn'}});
lastState=s;renderUnused();const D=s.defaults||{};fill('sysf',SYSF,s.sys,D.sys);fill('motf',MOTF,s.motor,D.motor);
const fs=$('sysf').querySelector('[name=failsafe_pos]');if(fs)fs.readOnly=true;
renderEnabled(s);
formsDone=true;
const p=s.esp.previous;$('prev').textContent=p?`Previous ESP32 firmware in the other slot: ${p.project} ${p.version} (${p.date})`:'No previous ESP32 firmware';$('rb').disabled=!p;
const u=s.update;if(!espUpload&&(u.active||u.result)){$('prog').value=u.percent;$('upd').textContent=u.active?`STM32 update: ${u.phase} ${u.percent}%`:`Last STM32 update: ${u.result||u.phase}`;
const ask=u.active&&u.phase.indexOf('RESET')>=0;$('upd').className=ask?'warn':'mut';$('upd').style.fontWeight=ask?'700':'';
if(ask)$('upd').textContent='Press the RESET button on the STM32 (BlackPill) now – waiting up to 30 s';$('updd').textContent=u.detail?('Bootloader entry: '+u.detail):''}
const e=s.esp;if(e.heap_size){const used=100-e.heap*100/e.heap_size,peak=100-e.heap_min*100/e.heap_size;$('heapbar').value=used;
$('heaptxt').textContent=`${used.toFixed(0)} % (${(e.heap/1024).toFixed(0)} of ${(e.heap_size/1024).toFixed(0)} kB free) · peak ${peak.toFixed(0)} % · largest free block ${(e.heap_block/1024).toFixed(0)} kB`}
const d=Object.assign({},s.link.stats,s.diag||{});$('diag').innerHTML=Object.entries(d).map(([k,v])=>`<span>${esc(k)}: <b>${esc(v)}</b></span>`).join('');
$('log').innerHTML=s.log.slice().sort((a,b)=>b.ms-a.ms).map(e=>`<tr><td class=mut>${(e.ms/1000).toFixed(0)} s</td><td>${e.src==='esp'?'ESP32':'STM32'}</td><td>${['error','warn','info','debug'][e.level]}</td><td>${esc(e.text)}</td></tr>`).join('')}
loadCfg().then(refresh);setInterval(refresh,2000);
</script></body></html>)HTML";

} // namespace app
