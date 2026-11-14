# ESP32: configuration, room control and Home Assistant integration

Status: **accepted**, phases 2b and 3 implemented · Phases 2b and 3 of [`../refactoring/PLAN.md`](../refactoring/PLAN.md)

This document defines what the ESP32 firmware v2 stores, how rooms are
controlled, how a minimum flow through the heating circuits is guaranteed
and how everything is exposed via MQTT / Home Assistant. KNX follows later
and reuses the same model.

Guiding principles:

- **The controller works on its own.** Room control runs on the ESP32;
  Home Assistant (and later KNX) is optional. HA can either set target
  temperatures for the ESP32's controller or take over the valve positions
  completely.
- **One place for names and assignments: the ESP32.** HA receives them via
  discovery.
- **Stable identifiers** (MAC address, channel number, 1-Wire ROM id) in
  topics and `unique_id`s; names are display text only, so renaming never
  breaks anything.

---

## 1. Concepts

```
temperature sources ──► zone (room) ──► valves (1..n channels) ──► flow guard ──► STM32
  1-Wire, MQTT, (KNX)     mode, setpoint,      min/max position,       min opening
                          PI controller        factor                 per valve and in total
```

| Term | Meaning |
|---|---|
| **Channel** | One motor output of the STM32 (1…12). |
| **Valve** | A channel with a name, limits and a zone. |
| **Zone** | A room or heating circuit with one controller. A zone has one or more valves, e.g. a bathroom with two floor heating circuits. A room with one valve is a zone with one valve. |
| **Temperature source** | A named temperature value: a 1-Wire sensor, an MQTT topic (Home Assistant, KNX gateway, …) or later a KNX group address. |

---

## 2. Configuration

### 2.1 Storage

- One JSON document `/config.json` on LittleFS with a `version` field.
- It is written atomically: first to `/config.json.tmp`, then renamed, and
  the previous file is kept as `/config.json.bak`. On load the firmware
  falls back to the backup and then to the defaults.
- Migrations run when `version` is older than the firmware.
- The legacy 1.x configuration is **not** taken over (decided). Only the
  network settings are read from the legacy NVS on the very first start,
  so the device stays reachable. After that they live in `/config.json`.
- Export and import of the whole file via the web page (backup / transfer).
- STM32 settings (motor parameters, link timeout, …) stay on the STM32 as
  today; the ESP32 only shows and edits them.

### 2.2 Contents

| Section | Fields |
|---|---|
| `device` | name (also the MQTT/HA device name), web user and password, time zone, NTP server |
| `network` | mode (auto / Ethernet / WiFi), DHCP or static (IP, mask, gateway, DNS), WiFi SSID and password |
| `mqtt` | enabled, broker host and port, user and password, client id (default from the MAC), base topic, HA discovery on/off, discovery prefix (`homeassistant`) |
| `syslog` | enabled, host, port, level (diagnostics) – later |
| `flow` | fallback position, minimum total opening, see 5 |
| `hvac` | mode auto/heat/cool, global supply temperature source, margin, see 4.5 |
| `sources[]` | temperature sources, see 3 |
| `zones[]` | zones, see 4 |
| `valves[12]` | name, zone, minimum and maximum position, factor, flow priority, see 4.3 |

**Dropped** compared with 1.x (decided):
- Pushover and e-mail notifications,
- Fahrenheit and the German number format,
- the MQTT format options (plain text, separate topics, …),
- the park position, which becomes the **fallback position**.

---

## 3. Temperature sources

Each source has:

| Field | Meaning |
|---|---|
| `id` | stable key (`s1`, `s2`, …), assigned by the ESP32, never reused |
| `name` | display name, e.g. "Bathroom", "Supply", "Bathroom return" |
| `type` | `onewire` (ROM id), `mqtt` (topic, optional JSON field), later `knx` (group address). An MQTT source without a topic listens on `<base>/source/<id>/set`. |
| `offset` | correction in K |
| `max_age_s` | after this time without a new value the source counts as **stale**; 0 = never stale |

1-Wire sensors found on the bus appear in a list and can be added with a
click. Sources do not need to belong to a zone (e.g. the supply
temperature).

### 3.1 Stale values

A value is stale if **no new value arrived** for `max_age_s`; for 1-Wire
this means no successful read. Defaults:

| Type | Default `max_age_s` | Reason |
|---|---|---|
| 1-Wire | 300 | read every 30 s |
| MQTT | 3600 | many publishers only send on change |

**A value that simply does not change** cannot be told apart from a
stable room temperature. The firmware does not try. Instead the documented
rule is: the source has to send regularly. Home Assistant's
`mqtt_statestream` publishes only on change, so an HA automation should
repeat the value, e.g. every 10 minutes, or `max_age_s` has to be set to 0
for that source.

---

## 4. Zones and valves

### 4.1 Zone

| Field | Meaning |
|---|---|
| `id`, `name` | stable key (`z1`, …) and display name, e.g. "Bathroom" |
| `mode` | `control`: the ESP32 controller sets the valves. `manual`: the valve positions come from outside (web, HA, KNX). |
| `setpoint` | heating target temperature in °C (5…30), stored, can be changed via web, HA or KNX |
| `cool_setpoint` | cooling target temperature in °C (15…35, default 24), see 4.5 |
| `enabled` | on/off (HA climate `off`). Off: the valves go to their minimum position. |
| `supply_source` | supply temperature of the zone (e.g. its distributor); empty: `hvac.supply_source`, see 4.5 |
| `control_sources` | up to 4 temperature sources used for control; the zone temperature is the mean of the valid ones (e.g. one room with two sensors) |
| `info_sources[]` | further sources shown with the zone, e.g. the return temperature of each circuit |
| `profile` | controller parameters: `floor` (floor heating), `radiator` or `custom` (see 4.4) |
| `fallback` | position used while none of the `control_sources` is valid; default: the global fallback position |

### 4.2 Valves of a zone

In `control` mode the controller computes **one** output (0…100 %) for the
zone. Each valve of the zone gets this output, multiplied by its `factor`
(default 100 %) and limited to its minimum and maximum position. The factor
allows a simple hydraulic balancing between the circuits of a room.

In `manual` mode every valve takes its own target position from outside.

### 4.3 Valve settings

| Field | Default | Meaning |
|---|---|---|
| `name` | "Channel n" | display name |
| `zone` | – | zone the valve belongs to; without a zone the valve is set directly, like a zone in `manual` mode |
| `min_pos` | 0 % | **minimum opening**, never undershot except while calibrating |
| `max_pos` | 100 % | maximum opening |
| `factor` | 100 % | scaling of the zone output |
| `flow_priority` | no | preferred valve for the minimum total opening (5) |

### 4.4 Controller

A PI controller per zone in `control` mode.

- **Time base:** monotonic (`esp_timer`), never the wall clock.
- **Sample time:** from the profile. A new target is only sent if it
  differs by at least 1 % from the last one, which saves motor movements.
- **Anti-windup:** conditional integration, i.e. no integration while the
  output is saturated in the direction of the error.
- **Bumpless switching** from `manual` to `control`: the integral starts
  from the current valve position.
- **No valid control source:** the controller holds its state and the zone
  goes to its fallback position. The fallback is a valve position: the
  factor is not applied, the limits are. A diagnostic flag is set and a log
  entry written. When values arrive again, control resumes; the first
  integration step after the pause is capped at two sample times.
- **Start-up:** a source without a value is *pending* for 120 s after the
  start. Meanwhile the valves keep their position (or the restored
  controller output); only after that a missing value leads to the
  fallback position. The controller starts bumpless from the current
  valve positions, so it waits until the STM32 has reported them.
- **Removing** a source or zone clears the references to it (zones, valves).
- **The controller state survives an ESP32 restart:** it is kept in RTC RAM
  as in 1.x, and the setpoint is stored in the configuration.

Profiles (starting values, to be tuned):

| Profile | Proportional band Xp | Integral time Ti | Sample time |
|---|---|---|---|
| `floor` | 2 K (50 %/K) | 3600 s | 300 s |
| `radiator` | 1.5 K (≈67 %/K) | 900 s | 60 s |
| `custom` | free | free | free |

---

### 4.5 Heating, cooling and pauses

The heat pump of the owner's house cycles most of the year, and in summer
the same pipes cool passively (ground loop). What the water delivers is
judged **per zone** from its supply temperature `S` and its room
temperature `T`, with `hvac.supply_margin_k` = `m` (default 2 K):

| Condition | Medium | Controller |
|---|---|---|
| `S ≥ T + m` | heat | PI towards `setpoint` |
| `S ≤ T − m` | cold | PI towards `cool_setpoint` (opens when the room is too warm) |
| in between | none | **paused** |

- Once heating (cooling), `m/2` is enough to stay in that state
  (hysteresis); a change must last 5 minutes (dwell time), so the pipe
  cooling down after a short stop does not pause the zone at once.
- **Paused:** the integral is frozen and the valves keep their position,
  so the controller does not wind up while the heat pump is off and the
  motors do not move.
- Switching between heating and cooling restarts the controller bumpless.
- Zones are independent: in summer an attic above its cooling setpoint
  cools while a cool ground floor pauses.
- `hvac.mode`: `auto` (default) as above; `heat` / `cool` allow only that
  medium (the other one pauses), e.g. switched by Home Assistant from the
  heat pump's operating mode.
- Without a supply sensor (or while it is stale): `auto` and `heat` heat,
  `cool` cools, nothing is paused — the behaviour without this feature.
- Dew point protection is left to the heat pump (it limits the supply
  temperature in cooling mode).

In Home Assistant the zone's `climate` gets the modes heat/cool/off and
the action heating/cooling/idle (phase 3).

## 5. Flow guard (minimum flow)

The heating has no buffer tank and no overflow valve, so a minimum flow
must be guaranteed whatever the controllers, HA or KNX demand.

1. **Per valve:** `min_pos` (4.3) is applied first.
2. **In total:** `flow.min_total` is the minimum **sum of the target
   positions** of all valves that are enabled, connected and free of
   faults, in percentage points (e.g. 150 = one and a half fully opened
   valves; 0 = off).
3. If the sum is below `min_total`, the deficit is spread:
   - first evenly over the valves with `flow_priority` (e.g. bathroom,
     hall), up to their `max_pos`;
   - then evenly over all other valves.
4. Valves that are not connected or in a fault state count as 0 %, so the
   others compensate for them.
5. The guard runs after every change and is shown:
   - *flow guard active*, with the amount added, on the web page and in HA;
   - each valve reports the **requested** position (controller, HA, …) and
     the **effective** one (sent to the STM32).

Note: valve opening and flow are not linear; the percentages are an
approximation that has to be tuned on the real installation.

**Without the ESP32:** before the STM32 failsafe starts, the last
positions are kept (they satisfy the guard); afterwards the failsafe
position applies (default fully open). The ESP32 writes its fallback
position to the STM32 as `failsafe_pos`, so both use the same value.

---

## 6. Order of evaluation

```
for every zone:
    control mode : output = PI(setpoint, mean of the valid control_sources)  or  fallback if stale
                   heating off → output = 0 (→ min_pos)
    manual mode  : per valve the last external target
for every valve:  target = clamp(output × factor, min_pos, max_pos)
flow guard:       raise targets until the sum ≥ min_total
send changed targets to the STM32
```

Command sources (web, MQTT/HA, later KNX) are equal; the last command
wins. In `control` mode external valve positions are rejected with a log
entry; only setpoint, heating on/off and mode are accepted.

---

## 7. MQTT

Base topic: `vdmot/<device-id>`, with `device-id` = `vdmot-` + the last 3
bytes of the Ethernet MAC in hex (both configurable). Payloads are JSON.
States are retained and published on change (checked every 2 s) and in
full every 5 minutes; commands are not retained. Valves are numbered 1…12.

| Topic | Direction | Content |
|---|---|---|
| `<base>/status` | out, LWT | `online` / `offline` |
| `<base>/link` | out | `online` / `offline` (STM32 link) |
| `<base>/state` | out | `{failsafe, flow_guard, flow_insufficient, flow_total, flow_added, esp_version, stm_version}` |
| `<base>/zone/<zid>/state` | out | `{mode, enabled, setpoint, cool_setpoint, temperature, supply, source, stale, medium, output, action}` |
| `<base>/zone/<zid>/climate` | out | `online` in mode `control`, `offline` in mode `manual` (availability of the climate entity) |
| `<base>/zone/<zid>/set` | in | any of `{"setpoint":21.5, "cool_setpoint":24, "enabled":false, "mode":"manual"}`; `enabled` also takes `"on"`/`"off"`/`"heat"`/`"cool"` |
| `<base>/valve/<n>/state` | out | `{name, zone, position, target, requested, effective, mode, fault, current_ma, controlled}` |
| `<base>/valve/<n>/set` | in | `{"position":40}` or a plain `40` (valves outside of a controlled zone), `{"calibrate":true}` |
| `<base>/source/<sid>/state` | out | `{name, value, age, status}` |
| `<base>/source/<sid>/set` | in | value of an MQTT source without its own topic (plain number) |

MQTT temperature sources (3) subscribe to the topic configured there, or
to `<base>/source/<sid>/set` if none is configured. Payloads that are not a
number (e.g. `unavailable`) are ignored. Commands with invalid values are
rejected as a whole and logged.

The client (PubSubClient, no TLS) runs in its own task; a broker that is
not reachable does not block the web page or the control.

### 7.1 Temperatures from Home Assistant

Create an MQTT source per room with an empty topic; the web page shows its
default topic (`<base>/source/<sid>/set`) below the field. In Home
Assistant import the blueprint
[`blueprints/automation/vdmot/send_temperature.yaml`](../../blueprints/automation/vdmot/send_temperature.yaml)
and create one automation per room: choose the sensor, paste the topic.
It publishes on every change, at the start of Home Assistant and every 5
minutes, so `max_age_s` can stay at its default; non-numeric states are
not sent.

## 8. Home Assistant (discovery)

Entity names: Home Assistant puts the device name in front. Sources have
an optional `short_name` that replaces their name there. Valve entities are
named "Valve <function> <suffix>" (target, position, state, fault, motor
current, calibrate); the suffix is the valve's `short_name`, or nothing if
it is the only valve of its zone, otherwise its name. Example: "VdMot
Office Valve position", "VdMot Bathroom Valve position front".

Discovery messages are retained, sent step by step after connecting, after
every configuration change and when Home Assistant announces
`<prefix>/status = online`. Entities of zones or sources removed while the
ESP32 runs are deleted; the `unique_id`s are `<client id>_<object>`, e.g.
`vdmot-a1b2c3_z1_climate`, so renaming never creates new entities.

**Availability:** `<base>/status` **and** `<base>/link` (mode `all`); the
climate additionally `<base>/zone/<zid>/climate`.

**Controller device** (`<client id>`, with firmware version and a link to
the web page):

| Entity | Content |
|---|---|
| `binary_sensor` STM32 link | connectivity (only needs the ESP32) |
| `binary_sensor` Failsafe | problem |
| `binary_sensor` Flow guard intervening | the flow guard currently raises valves |
| `number` Minimum total opening / Fallback position | configuration (`<base>/set` `{"min_total_pct":150}`, `{"fallback_pct":100}`) |
| `select` Heating / cooling | `auto` / `heat` / `cool` (`hvac.mode`, command `<base>/set`) |
| `sensor` Total valve opening | % (diagnostic) |
| `sensor` per temperature source | °C |

**Zone device** (`<client id>_<zid>`, named "<device name> <zone name>",
e.g. "VdMot Bathroom", `via_device` the controller, can be assigned to an
area):

| Entity | Content |
|---|---|
| `climate` (named like the zone) | mode `heat_cool` = the zone controls (mode `control`) with both setpoints as a range (low = heating, high = cooling; the supply temperature decides which is in use); mode `off` = control off (mode `manual`), the valves keep their position and are set with "Valve target"; current temperature; action heating / cooling / idle / off |
| `sensor` Temperature | zone temperature |
| `sensor` Controller output | % (before factor and limits) |
| `sensor` Supply temperature | diagnostic |
| `number` Heating setpoint / Cooling setpoint | configuration, disabled by default (the climate sets both) |
| `binary_sensor` Temperature missing | problem |
| `sensor` State | heating, cooling, paused, fallback, … (diagnostic) |
| `sensor` Control sources | e.g. "Office (MQTT), Return (1-Wire, stale)" (diagnostic) |
| `sensor` per further temperature | the zone's `info_sources` |

The zone's on/off (`enabled`, valves at their minimum opening) is set on
the web page or via MQTT; it is not part of the climate.

**Per enabled valve** (on its zone's device, or on the controller without a
zone; channels disabled on the STM32 get no entities): `number` Valve
target (slider 0…100 %, the requested position; **unavailable while the
valve belongs to a zone in mode `control`**), `sensor` Valve position
(actual, %), `sensor` Valve state (idle, opening, closing, fault – …;
diagnostic), `binary_sensor` Valve fault (attributes: the whole valve
state), `sensor` Valve motor current and `button` Valve calibrate (both
disabled by default). Entities of earlier versions (valve entity, zone
mode select) are deleted.

**If HA controls the rooms itself:** set the zone to `manual` and use HA's
own thermostat with the `valve` entities.

Presets (eco/comfort) and schedules are not part of v1; HA or KNX change
the setpoints for that.

---

## 9. Web page (phase 2b)

New sections in the existing style; the full redesign follows in phase 4:
- zones (members, mode, setpoint, sources, profile),
- valves (name, limits, factor, priority),
- temperature sources (1-Wire list with "add", MQTT sources),
- flow guard,
- network, MQTT, device, syslog,
- configuration export/import.

## 10. Phasing

| Step | Content |
|---|---|
| 2b-1 | configuration store, migration of the network settings, settings pages for network/device/MQTT |
| 2b-2 | temperature sources, zones, valves, flow guard, PI controller, host tests with a room model |
| 3 | MQTT and HA discovery as described in 7 and 8 |
| later | KNX adapter on the same model (setpoint, actual value, output, mode per zone; sources of type `knx`) |

## 11. Decisions and later ideas

Decided:
- `min_pos` and `min_total` are **off by default** (0) and are set during
  commissioning.

Later, optional (not part of v1):
- window contact per zone (setpoint lowered while open);
- supply/return temperature difference per zone (until then the info
  sources show both temperatures side by side).
