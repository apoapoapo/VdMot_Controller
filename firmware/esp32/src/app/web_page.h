/*
 * Web page: overview, room setup, settings and maintenance. Mainly for
 * setting up and diagnosing; daily use is meant to happen in Home Assistant.
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
.pill{padding:2px 8px;border-radius:10px;border:1px solid var(--line)}.pill.ok{border-color:var(--ok)}.pill.bad{border-color:var(--bad)}.pill.warn{border-color:var(--warn)}.ok{color:var(--ok)}.bad{color:var(--bad)}.warn{color:var(--warn)}
input[type=number]{width:70px}input,button,select{font:inherit;padding:3px 6px;border:1px solid var(--line);border-radius:5px;background:var(--card);color:var(--fg)}
input.w{width:220px}input.s{width:110px}input.n{width:170px}
p.intro{margin:0 0 8px;color:var(--mut)}
details.help{margin:4px 0 8px;color:var(--mut)}details.help summary{cursor:pointer;color:var(--acc)}
details.help ul{margin:4px 0;padding-left:18px}details.help li{margin:3px 0}
details.exp{margin-top:10px;border-top:1px solid var(--line);padding-top:8px}details.exp>summary{cursor:pointer;font-weight:600}
th[title]{cursor:help;text-decoration:underline dotted;text-underline-offset:3px}
.fld{display:flex;flex-direction:column;gap:1px;padding:3px 0}.fld small.hint{color:var(--mut);font-size:12px}
.tags{display:flex;flex-wrap:wrap;gap:4px;align-items:center;min-width:160px;max-width:300px}
.tag{border:1px solid var(--line);border-radius:10px;padding:1px 4px 1px 8px;white-space:nowrap}
.tag b{cursor:pointer;padding:0 4px;color:var(--mut)}.tag b:hover{color:var(--bad)}.tags select{padding:1px 4px}
button{cursor:pointer}button.p{background:var(--acc);color:#fff;border-color:var(--acc)}button:disabled,input:disabled{opacity:.45;cursor:default}
.row{display:flex;flex-wrap:wrap;gap:8px;align-items:center;margin:6px 0}.mut{color:var(--mut)}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(260px,1fr));gap:4px 16px}
label{display:flex;justify-content:space-between;align-items:center;gap:8px}progress{width:200px}
@media(max-width:700px){main{padding:8px}section{padding:8px}header{padding:8px}td,th{padding:3px 4px}.grid{grid-template-columns:1fr}}
.chg input{border-color:var(--warn)}small.def{color:var(--warn);font-size:12px}
#toast{position:fixed;left:50%;bottom:20px;transform:translateX(-50%);background:var(--fg);color:var(--bg);padding:8px 14px;border-radius:6px;display:none;max-width:90vw;z-index:9}
</style></head><body>
<div id="toast"></div>
<header><h1 id="title">VdMot Controller</h1>
<span id="link" class="pill">…</span><span id="netp" class="pill"></span><span id="mqttp" class="pill" style="display:none"></span>
<span id="fsp" class="pill bad" style="display:none" title="the STM32 moved the valves to the failsafe position (no ESP32 commands)">FAILSAFE</span>
<span id="ver" class="mut"></span><span id="net" class="mut"></span>
<span id="err" class="bad"></span></header>
<nav id="nav"><a href="#ov">Overview</a><a href="#rooms">Rooms &amp; valves</a><a href="#set">Settings</a><a href="#sys">System</a></nav>
<main>
<div class="tab" id="tab-ov">
<section><h2>Zones</h2><table><thead><tr><th title="below the name: the sources of the control temperature">Zone</th><th class="num" title="control temperature: mean of the valid control sources">Temperature °C</th>
<th class="num" title="decides whether heat or cold is supplied">Supply °C</th><th title="used while heat is supplied (bold = in use)">Heat setpoint °C</th><th title="used while cold is supplied (bold = in use)">Cool setpoint °C</th>
<th title="off: the valves go to their minimum opening">On</th><th title="control: the ESP32 sets the valves; manual: they are set by hand or by Home Assistant">Mode</th>
<th class="num" title="controller output before factor and limits">Output %</th><th>State</th></tr></thead><tbody id="zones"></tbody></table>
<div class="row mut" id="nozones">No zones yet – create them under “Rooms &amp; valves”.</div>
<div class="row" id="flow"></div></section>
<section><h2>Valves</h2>
<div class="row"><button onclick="calibrate('all')">Calibrate all</button>
<button onclick="act('detect','all')">Detect all</button><button onclick="act('stop','all')">Stop all</button>
<label style="gap:4px"><input type=checkbox id=showdis onchange="refresh()"> show disabled channels <span id=ndis class=mut></span></label></div>
<table><thead><tr><th>#</th><th>Name</th><th>Zone</th><th>State</th><th class="num" title="position reported by the STM32">Actual %</th><th class="num" title="target on the STM32">Target %</th>
<th class="num" title="requested by the controller or a command → sent after min/max and the flow guard">Requested → effective %</th>
<th title="only for valves without a zone or in a zone in mode manual">Set</th><th class="num" title="motor current while moving">mA</th>
<th title="cal = calibrated, end = at an endstop, pos? = position not known yet, failsafe = failsafe target">Flags</th>
<th class="num" title="pulses for a full opening / closing stroke (learned by the calibration)">Strokes open/close</th><th class="num">Moves</th><th></th></tr></thead>
<tbody id="valves"></tbody></table></section>
</div>

<div class="tab" id="tab-rooms">
<section><h2>Zones</h2>
<p class="intro">A zone is a room or heating circuit with one controller; its valves are assigned under Valves below. Hover over a column title for an explanation.</p>
<details class="help"><summary>Help</summary><ul>
<li><b>Mode</b> control: the ESP32 sets the valves from the temperature. manual: the valve positions are set by hand or by Home Assistant (climate off).</li>
<li><b>Heat / Cool</b>: target temperature while the water supplies heat or cold (Settings → Heating / cooling decides which). <b>On</b> unchecked: the valves go to their minimum opening.</li>
<li><b>Control temperature</b>: one or more sources; the mean of the valid ones is used. <b>Further temperatures</b> are only shown with the zone, e.g. return temperatures.</li>
<li><b>Profile</b>: floor (Xp 2 K, Ti 3600 s, sample 300 s) for slow floor heating, radiator (Xp 1.5 K, Ti 900 s, sample 60 s), or custom values.</li>
<li><b>Xp</b> (proportional band): the temperature difference that changes the output by 100 %. Smaller = stronger reaction. <b>Ti</b> (integral time): how fast a remaining difference is corrected; 0 = off. <b>Sample</b>: how often the controller computes a new output.</li>
<li><b>Supply temperature</b>: the supply sensor of this zone, e.g. its distributor; empty = the global one. <b>Fallback</b>: valve position while no control temperature is valid; empty = global.</li></ul></details>
<table><thead><tr><th>Name</th><th title="control: the ESP32 sets the valves; manual: set by hand or by Home Assistant">Mode</th><th title="setpoint while heat is supplied">Heat °C</th><th title="setpoint while cold is supplied">Cool °C</th>
<th title="off: the valves go to their minimum opening">On</th><th title="the mean of the valid sources is used">Control temperature</th><th title="only shown, e.g. return temperatures">Further temperatures</th>
<th title="controller parameters: floor, radiator or custom">Profile</th><th title="empty = the global supply sensor">Supply temperature</th>
<th title="proportional band: temperature difference for 100 % output">Xp K</th><th title="integral time, 0 = off">Ti s</th><th title="how often a new output is computed">Sample s</th>
<th title="valve position while no control temperature is valid; empty = global">Fallback %</th><th></th></tr></thead><tbody id="ezones"></tbody></table>
<div class="row"><button onclick="addZone()">Add zone</button><button class="p" onclick="saveZones()">Save zones</button></div></section>
<section><h2>Valves</h2>
<p class="intro">One row per motor output. Valves without a zone are set directly in the overview.</p>
<details class="help"><summary>Help</summary><ul>
<li><b>Enabled</b>: switch off outputs without a valve; the STM32 then neither detects nor moves them, and Home Assistant gets no entities for them.</li>
<li><b>Short name (HA)</b>: appended to the valve entities in Home Assistant, e.g. "front" → "VdMot Office Valve position front". Empty: nothing if the valve is the only one of its zone, otherwise the name.</li>
<li><b>Min / Max</b> limit every target; min is the minimum opening, also with the zone off. <b>Factor</b> scales the zone output (simple hydraulic balancing between the circuits of a zone).</li>
<li><b>Flow priority</b>: these valves are opened first to reach the minimum total opening (Settings → Flow guard).</li></ul></details>
<table><thead><tr><th>#</th><th title="off: the STM32 ignores this output">Enabled</th><th>Name</th><th title="name suffix in Home Assistant">Short name (HA)</th><th>Zone</th>
<th title="minimum opening">Min %</th><th title="maximum opening">Max %</th><th title="scales the zone output">Factor %</th><th title="opened first for the minimum total opening">Flow priority</th></tr></thead><tbody id="evalves"></tbody></table>
<div class="row"><button class="p" onclick="saveValves()">Save valves</button></div></section>
<section><h2>Temperature sources</h2>
<p class="intro">Temperatures from 1-Wire sensors (offered below the table when found on the bus) or from MQTT, e.g. Home Assistant.</p>
<details class="help"><summary>Help</summary><ul>
<li><b>MQTT topic</b>: the topic the value is published on, e.g. by zigbee2mqtt. Empty = the default topic shown below the field. Home Assistant: import the blueprint <code>blueprints/automation/vdmot/send_temperature.yaml</code> from the repository and create one automation per room with the sensor and this topic.</li>
<li><b>JSON field</b>: only if the payload is JSON, e.g. <code>{"temperature":21.5,"humidity":40}</code> → <code>temperature</code>; nested fields with a dot (<code>sensor.temp</code>). Empty = the payload is a plain number.</li>
<li><b>Short name (HA)</b>: optional name in Home Assistant, which puts the device name in front, e.g. "Return" on the zone "Office" → "VdMot Office Return".</li>
<li><b>Offset</b>: correction added to the value. <b>Max age</b>: after this time without a new value the source counts as stale and zones using only stale sources go to their fallback position (0 = never). Publishers that only send on change need a regular resend (the blueprint does that).</li></ul></details>
<table><thead><tr><th>Name</th><th title="name in Home Assistant, empty = the name">Short name (HA)</th><th>Type</th><th>ROM id / MQTT topic</th><th title="only for JSON payloads">JSON field</th>
<th title="added to the value">Offset K</th><th title="stale after this time without a new value; 0 = never">Max age s</th><th class="num">Value °C</th><th>State</th><th>Used by</th><th></th></tr></thead><tbody id="esources"></tbody></table>
<div class="row"><button onclick="addSource('mqtt')">Add MQTT source</button><button class="p" onclick="saveSources()">Save sources</button></div>
<div class="row mut" id="unused"></div></section>
</div>

<div class="tab" id="tab-set">
<section><h2>Heating / cooling</h2>
<p class="intro">Each zone compares its supply temperature with its room temperature: clearly above = heat is supplied, clearly below = cold, in between = nothing, and the controller pauses (valves keep their position).</p>
<div class="grid" id="f-hvac"></div>
<div class="row"><button class="p" onclick="saveForm('hvac')">Save</button></div></section>
<section><h2>Flow guard and fallback</h2>
<p class="intro">For heating systems without a buffer tank or bypass: keeps a minimum flow whatever the zones demand.</p>
<div class="grid" id="f-flow"></div>
<div class="row"><button class="p" onclick="saveForm('flow')">Save</button></div></section>
<section><h2>Device</h2><div class="grid" id="f-device"></div>
<div class="row"><button class="p" onclick="saveForm('device')">Save</button></div></section>
<section><h2>Network</h2><div class="grid" id="f-network"></div>
<div class="row"><button class="p" onclick="saveForm('network')">Save</button><span class="mut">Takes effect after a restart. If no network comes up, the ESP32 opens the access point VdMot-XXXX (password vdmot-setup).</span></div></section>
<section><h2>MQTT</h2><div class="grid" id="f-mqtt"></div>
<div class="row"><button class="p" onclick="saveForm('mqtt')">Save</button></div></section>
<section><h2>STM32 settings</h2>
<p class="intro">Stored on the STM32, kept over updates. Orange: differs from the default.</p>
<form id="sys" onsubmit="save('system',this);return false"><div class="grid" id="sysf"></div>
<div class="row"><button class="p">Save system settings</button><button type="button" onclick="defaults('sysf','sys')">Load defaults</button></div></form>
<details class="exp"><summary>Motor parameters (expert)</summary>
<p class="intro">Endstop and fault detection of the valve motors. The defaults suit the HmIP-VdMot; change them only if valves report false endstops or faults, and one value at a time.</p>
<form id="mot" onsubmit="save('motor',this);return false"><div class="grid" id="motf"></div>
<div class="row"><button class="p">Save motor parameters</button><button type="button" onclick="defaults('motf','motor')">Load defaults</button></div></form></details>
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
</div><p class="intro">The STM32 enters the failsafe after its link timeout (default 300 s; can be lowered in the STM32 settings) and leaves it when the link resumes.</p></section>
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
/* [key, label, help] */
const SYSF=[['link_timeout_s','Link timeout s','If the STM32 hears nothing from the ESP32 for this long, it moves all valves to the failsafe position. 0 = never.'],
['failsafe_pos','Failsafe position ‰','Follows Settings → Fallback position (here in 0.1 %).'],
['startup_pos','Startup position ‰','Target after an STM32 restart until the ESP32 sends targets. 65535 = the last target.'],
['sensor_interval_s','1-Wire interval s','How often the 1-Wire sensors are read.'],
['recal_after_moves','Recalibrate after moves','Automatic recalibration after this many moves. 0 = off.'],
['recal_after_days','Recalibrate after days','Automatic recalibration after this many days of operation. 0 = off.']];
const MOTF=[['endstop_factor_open','Endstop factor opening (×0.1)','Endstop when the current exceeds the mean current of the move × factor/10 (17 = 1.7×). Higher: less sensitive (against false endstops of stiff valves); lower: stops earlier.'],
['endstop_factor_close','Endstop factor closing (×0.1)','As above, while closing.'],
['endstop_abs_ma','Endstop current limit mA','This current always counts as endstop (safety limit).'],
['inrush_blank_ms','Inrush blanking ms','The current peak when the motor starts is ignored for this time.'],
['opencircuit_ma10','Open circuit threshold (0.1 mA)','Below this current no motor is assumed …'],
['opencircuit_ms','Open circuit time ms','… if it lasts this long without interruption.'],
['move_timeout_s','Move timeout s','Longest single move; then the fault "timeout".'],
['min_stroke_counts','Minimum stroke pulses','A calibration with fewer pulses fails (HmIP-VdMot: about 3100–4400).'],
['max_calib_retries','Calibration retries','Retries after a failed calibration.']];
/* [key, label, type, help] */
const FORMS={hvac:[['mode','Mode','sel:auto|heat|cool','auto: heat or cool from the supply temperature. heat / cool: only this one, the other pauses (e.g. set by Home Assistant from the heat pump).'],
['supply_source','Supply temperature','src','Default for all zones; a zone can use its own (Rooms & valves). Without one, zones never pause.'],
['supply_margin_k','Margin K','num','Supply at least this far above / below the room = heat / cold. A change must last 5 min; once active, half the margin keeps it.']],
flow:[['fallback_pct','Fallback position %','num','Valve position while a zone has no valid temperature; also the STM32 failsafe position when the ESP32 is gone.'],
['min_total_pct','Minimum total opening %','num','Minimum sum of all valve positions in percentage points (150 = 1½ open valves). Flow priority valves are opened first; faulty valves do not count. 0 = off.']],
device:[['name','Name','text','Also the host name and the device name in Home Assistant.'],['web_user','Web user','text','Empty = no login.'],['web_pwd','Web password','pwd','Leave empty to keep it.'],
['tz','Time zone','text','POSIX format; Germany: CET-1CEST,M3.5.0,M10.5.0/3'],['ntp','NTP server','text','']],
network:[['mode','Interface','sel:auto|ethernet|wifi','auto: Ethernet, WiFi if Ethernet does not come up.'],['dhcp','DHCP','bool','Off: the addresses below are used.'],['ip','IP address','text',''],['mask','Subnet mask','text',''],
['gateway','Gateway','text',''],['dns','DNS server','text',''],['ssid','WiFi SSID','text',''],['wifi_pwd','WiFi password','pwd','Leave empty to keep it.']],
mqtt:[['enabled','Enabled','bool',''],['host','Broker host','text','Host name or IP address; no TLS.'],['port','Port','num',''],['user','User','text',''],['password','Password','pwd','Leave empty to keep it.'],
['client_id','Client id','text','Empty = vdmot- plus the end of the MAC address. Home Assistant identifies the device by it: changing it creates new entities.'],
['base_topic','Base topic','text','Empty = vdmot/<client id>.'],['discovery','Home Assistant discovery','bool','Creates a device for the controller and one per zone in Home Assistant.'],
['discovery_prefix','Discovery prefix','text','Usually homeassistant.']]};
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
function calibrate(ch){if(confirm(ch==='all'?'Calibrate all enabled valves? Each one runs fully open and closed, one after another (about 2 minutes per valve); calibrated valves wait fully open.'
:`Calibrate valve ${ch+1}? It runs fully open and closed (about 2 minutes).`))act('calibrate',ch)}
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
function fill(id,fields,vals,defs){const g=$(id);if(!formsDone)g.innerHTML=fields.map(([k,l,h])=>`<div class=fld data-k=${k}><label><span>${esc(l)} <small class=def></small></span><input type=number name=${k}></label><small class=hint>${esc(h)}</small></div>`).join('');
if(!vals)return;fields.forEach(([k])=>{const lab=g.querySelector(`[data-k=${k}]`);const diff=defs&&vals[k]!==defs[k];
lab.classList.toggle('chg',!!diff);lab.querySelector('small.def').textContent=diff?`(default ${defs[k]})`:''});
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
<td><input class=s data-k=short_name value="${esc(v.short_name||'')}"></td>
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
<td><input class=n data-k=name value="${esc(s.name)}"></td><td><input class=n data-k=short_name value="${esc(s.short_name||'')}" placeholder="${esc(s.name)}"></td><td>${m?'MQTT':'1-Wire'}</td>
<td>${m?`<input class=w data-k=topic value="${esc(s.topic)}" placeholder="default topic">${s.id&&!s.topic?`<div class=mut style="font-size:12px;user-select:all">${esc((CFG.mqtt_base||'vdmot/…')+'/source/'+s.id+'/set')}</div>`:''}`:esc(s.rom)}</td>
<td>${m?`<input class=s data-k=field value="${esc(s.field)}" placeholder="plain number">`:''}</td>
<td><input type=number data-k=offset min=-10 max=10 step=0.1 value=${s.offset}></td>
<td><input type=number data-k=max_age_s min=0 value=${s.max_age_s}></td>
<td class=num id="sv_${esc(s.id)}"></td><td id="ss_${esc(s.id)}" class=mut>${s.id?'':'not saved'}</td><td class=mut>${esc(s.id?usedBy(s.id):'')}</td>
<td><button onclick="collectSources();ED.sources.splice(${i},1);renderSources()">Delete</button></td></tr>`}).join('')||'<tr><td colspan=11 class=mut>No sources</td></tr>';renderUnused()}
function collectSources(){document.querySelectorAll('#esources tr[data-i]').forEach(tr=>Object.assign(ED.sources[tr.dataset.i],rowVals(tr)))}
function addSource(type,rom){collectSources();ED.sources.push(type==='mqtt'?{name:'MQTT '+(ED.sources.length+1),type,topic:'',field:'',offset:0,max_age_s:3600}
:{name:'Sensor '+rom.slice(-5),type,rom,offset:0,max_age_s:300});renderSources()}
function saveSources(){collectSources();saveSection('sources',ED.sources.map(s=>{const o=Object.assign({},s);if(!o.id)delete o.id;return o}),'Sources saved')}
const isTemp=k=>k===1||k===4;
function renderUnused(){if(!lastState||!CFG)return;const used=new Set(ED.sources.filter(s=>s.type==='onewire').map(s=>s.rom));
const un=lastState.sensors.filter(x=>isTemp(x.kind)&&!used.has(x.rom));
$('unused').innerHTML=un.length?'1-Wire sensors not used yet: '+un.map(x=>`<button onclick="addSource('onewire','${x.rom}')">${x.rom} (${(x.value/1000).toFixed(1)} °C) – add</button>`).join(' '):''}
function renderForm(sec){const v=CFG[sec];$('f-'+sec).innerHTML=FORMS[sec].map(([k,l,t,h])=>{let i;
if(t==='bool')i=`<input type=checkbox data-k=${k}${v[k]?' checked':''}>`;
else if(t==='pwd')i=`<input type=password class=s data-k=${k} placeholder="${v[k+'_set']?'unchanged':'not set'}" autocomplete=new-password>`;
else if(t.startsWith('sel:'))i=`<select data-k=${k}>${t.slice(4).split('|').map(o=>opt(o,o,v[k]===o)).join('')}</select>`;
else if(t==='src')i=`<select data-k=${k}>${opt('','– none –',!v[k])}${CFG.sources.map(s=>opt(s.id,srcLabel(s),s.id===v[k])).join('')}</select>`;
else if(t==='num')i=`<input type=number step=any data-k=${k} value="${esc(v[k])}">`;else i=`<input class=s data-k=${k} value="${esc(v[k])}">`;
return `<div class=fld><label><span>${esc(l)}</span>${i}</label>${h?`<small class=hint>${esc(h)}</small>`:''}</div>`}).join('')}
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
zs.forEach(z=>{const c=$('zr'+z.id).cells;c[0].innerHTML=esc(z.name)+(z.sources?`<br><small class=mut>${esc(z.sources)}</small>`:'');
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
const n=s.net,np=$('netp'),mq=$('mqttp');
np.textContent=(n.interface==='none'?'no network':n.interface==='ap'?'setup AP '+n.ap_ssid:n.interface+' '+n.ip)+(n.ap&&n.interface!=='ap'?' · AP '+n.ap_ssid:'');
np.className='pill '+(n.interface==='ethernet'||n.interface==='wifi'?'ok':n.interface==='ap'?'warn':'bad');
mq.style.display=n.mqtt==='disabled'?'none':'';mq.textContent='MQTT '+n.mqtt;mq.className='pill '+(n.mqtt==='connected'?'ok':'warn');
$('fsp').style.display=s.valves.some(v=>v.flags&16)?'':'none';
$('net').textContent=n.time||'';$('err').textContent=s.error||'';
$('cfginfo').textContent='Configuration at start: '+s.config;
renderZoneStatus(s);
const zname=id=>{const z=s.ctl.zones.find(x=>x.id===id);return z?z.name:''};
/* the valve table is built once; afterwards only the cells are updated, so
 * an input that has the focus is not replaced */
if($('valves').rows.length!==s.valves.length)$('valves').innerHTML=s.valves.map(v=>`<tr id=vr${v.ch}><td>${v.ch+1}</td><td></td><td></td><td></td><td class=num></td><td class=num></td><td class=num></td>
<td><input id=t${v.ch} type=number min=0 max=100 step=0.5 oninput="editing.add('t${v.ch}')"><button id=b${v.ch} onclick="setT(${v.ch})">Set</button></td>
<td class=num></td><td></td><td class=num></td><td class=num></td><td><button onclick="calibrate(${v.ch})">Calibrate</button></td></tr>`).join('');
const showDis=$('showdis').checked;let nDis=0;
s.valves.forEach(v=>{const row=$('vr'+v.ch),c=row.cells;const cv=s.ctl.valves[v.ch];
if(v.mode===0)nDis++;row.style.display=v.mode===0&&!showDis?'none':'';c[1].textContent=cv.name;c[2].textContent=zname(cv.zone);
c[3].className=v.mode===8?'bad':(v.mode===2||v.mode===0?'mut':'');
c[3].textContent=(MODES[v.mode]||v.mode)+(v.fault?' – '+FAULTS[v.fault]:'');c[4].textContent=pct(v.actual);c[5].textContent=pct(v.target);
c[6].textContent=cv.effective===undefined?'–':(cv.requested!==cv.effective?pct(cv.requested)+' → ':'')+pct(cv.effective);
const i=$('t'+v.ch);if(!editing.has('t'+v.ch)&&document.activeElement!==i)i.value=(cv.effective??v.target)/10;
i.disabled=$('b'+v.ch).disabled=cv.controlled;i.title=cv.controlled?'set by the zone controller':'';
c[8].textContent=(v.current/10).toFixed(1);c[9].textContent=flags(v.flags);c[10].textContent=(v.open_counts??'')+'/'+(v.close_counts??'');c[11].textContent=v.moves??''});
$('ndis').textContent=nDis?`(${nDis})`:'';
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
lastUptime=s.esp.uptime_s;if(location.hash==='#sys')loadLog()}
/* the log has its own request, only while the System tab is open */
let lastUptime=0;
async function loadLog(){let l;try{const r=await fetch('/api/log');if(!r.ok)return;l=await r.json()}catch(e){return}
/* log times: ESP32 milliseconds since start → clock time of the browser */
const t0=Date.now()-lastUptime*1000;
$('log').innerHTML=l.sort((a,b)=>b.ms-a.ms).map(e=>`<tr><td class=mut>${new Date(t0+e.ms).toLocaleTimeString()}</td><td>${e.src==='esp'?'ESP32':'STM32'}</td><td class="${['bad','warn','',''][e.level]}">${['error','warn','info','debug'][e.level]}</td><td>${esc(e.text)}</td></tr>`).join('')}
loadCfg().then(refresh);setInterval(refresh,2000);
</script></body></html>)HTML";

} // namespace app
