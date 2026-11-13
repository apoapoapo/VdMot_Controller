# VdMot Link – ESP32 ⇄ STM32 protocol

Version: **1** (draft) · Date: 2026-11-13

This document specifies the protocol on the UART link between the ESP32
(WT32-ETH01) and the STM32. It replaces the text protocol of firmware 1.x
(`stgtp`, `gvlvd`, …). There is no compatibility with 1.x.

---

## 1. Roles and principles

- **ESP32 = master.** Only the ESP32 sends requests. The STM32 answers every
  request with exactly one response.
- **The STM32 reports changes on its own.** State changes are sent as
  **events** without being asked (e.g. "valve 3 reached its target", "new
  temperature readings"). Polling is only a safety net.
- **The STM32 makes no decisions.** It holds no sensor→valve mapping and no
  names. It knows channels 0…11 and 1-Wire sensors by their 64-bit ROM id.
- **Every request is either idempotent or protected against repetition**
  (see 4.3), so retries are safe.
- **All multi-byte values are little-endian.** Serialization is done field by
  field, never by `memcpy` of C structs (no dependency on padding/compiler).

---

## 2. Physical layer

| Parameter | Value |
|---|---|
| STM32 | USART1, PA9 (TX) / PA10 (RX) |
| ESP32 | `Serial2`, GPIO17 (TX) / GPIO5 (RX) |
| Format | 8N1, no flow control |
| Baud rate | 115200 (default); possibly 460800 later via `SET_LINK` – not part of v1 |
| Reset / boot | The ESP32 drives the STM32's NRST (GPIO15) and BOOT0 (GPIO14), as today |

The STM32 firmware is updated via the **ROM bootloader** (AN3155) on the
same UART. **BOOT0 is not wired to the ESP32** on the C2/C3 boards (GPIO14
only goes to an open solder pad), so the bootloader is entered by software,
compatible with the legacy 1.x firmware:

1. The ESP32 resets the STM32 (NRST) and sends `DEADBEEF\r\n` at
   115200 baud **8E1** every 250 ms.
2. During the first **2 seconds** after a reset the STM32 firmware listens
   for this string (legacy 1.x: 3 seconds). On a match it answers
   `BEEFIT\r\n` and jumps to the ROM bootloader.
3. The ESP32 then speaks the ST bootloader protocol (sync byte `0x7F`).

If the reset does not reach the STM32 (jumper X20 open or reset circuit not
populated), the ESP32 asks the user to press the RESET button of the
BlackPill and keeps sending the request for 30 s. After a successful update
the ESP32 starts the new firmware with the bootloader's GO command; the
firmware sets its vector table itself (`USER_VECT_TAB_ADDRESS`).

If the STM32 is already in the bootloader, step 1 is skipped. If no
application answers (e.g. after an interrupted update), the ESP32 finally
tries BOOT0 + reset; that only works if BOOT0 is wired via the solder pad or
the BOOT0 button of the BlackPill is held.

---

## 3. Framing

### 3.1 Layout

On the wire every frame is **COBS encoded** and **terminated by a `0x00` byte**.
`0x00` never occurs inside the encoded content, so after any error the receiver
resynchronizes at the next `0x00`.

Decoded content:

```
 0        1        2        3        4 … 4+n-1     4+n   4+n+1
+--------+--------+--------+--------+-------------+------+------+
| ver    | kind   | seq    | msg    | payload (n) | crc_lo crc_hi|
+--------+--------+--------+--------+-------------+------+------+
```

| Field | Size | Meaning |
|---|---|---|
| `ver` | u8 | Protocol version, currently `1`. Frames with a different version are dropped (counter `rx_bad_version`). |
| `kind` | u8 | `0x01` REQUEST, `0x02` RESPONSE, `0x03` EVENT. Other values: drop. |
| `seq` | u8 | REQUEST: assigned by the ESP (1…255, `0` is reserved). RESPONSE: copied from the request. EVENT: the STM32's own counter (1…255, wrapping). |
| `msg` | u8 | Message id (chapters 5/6). A RESPONSE carries the same id as its request. |
| `payload` | 0…240 | message specific |
| `crc` | u16 | CRC-16/CCITT-FALSE (poly `0x1021`, init `0xFFFF`, no reflection, no xorout) over `ver` … end of `payload` |

- The payload length follows from the decoded frame length (`len - 6`); there
  is no separate length field.
- Maximum decoded frame length: **246 bytes**, encoded ≤ 248 bytes including `0x00`.
- Frames shorter than 6 bytes, longer than 246 bytes, with a COBS or CRC
  error, an unknown `kind` or `seq` = 0 are **silently dropped**. Each error type has its own counter (see
  `GET_DIAG`). Corrupted frames are **never** answered, because their `seq`
  cannot be trusted.

### 3.2 Receive buffer

- Each side keeps a frame buffer of at least 256 bytes. If it fills up without
  a `0x00`, everything up to the next `0x00` is discarded (`rx_overflow`).
- The STM32 receives UART bytes by interrupt or DMA into a ring buffer of
  ≥ 512 bytes. Parsing happens in the main loop. **All** complete frames are
  processed in each loop pass.

---

## 4. Flow

### 4.1 Request / response

- The ESP has **at most one outstanding request** (stop-and-wait). That is
  enough for the small data volume and keeps both sides simple.
- The STM32 answers within **20 ms** (target), at the latest after 50 ms.
  Actions that take longer (moving, calibrating, 1-Wire search) are only
  **accepted**. Their result arrives later as an event.
- **Timeout on the ESP side: 100 ms.** After that the same request is repeated
  with the **same `seq`**, at most 3 times.
- After 3 failed retries the link is considered **down** (`LINK_DOWN`):
  - The ESP tries `HELLO` every 1 s.
  - After 30 s without an answer it hard-resets the STM32 via NRST, at most
    once every 5 min.
  - It reports `LINK_DOWN` to the data model. Home Assistant then shows the
    valves as "unavailable".

### 4.2 Status code of every response

The **first payload byte of every response** is a status code:

| Code | Name | Meaning |
|---|---|---|
| `0x00` | `OK` | executed or accepted |
| `0x01` | `ERR_UNKNOWN_MSG` | unknown message id |
| `0x02` | `ERR_BAD_LEN` | payload length does not match |
| `0x03` | `ERR_RANGE` | value out of range; the second byte holds the index of the first invalid field |
| `0x04` | `ERR_CHANNEL` | channel invalid or disabled |
| `0x05` | `ERR_BUSY` | resource busy (e.g. a 1-Wire search is already running) |
| `0x06` | `ERR_STATE` | not allowed in the current state (e.g. valve not calibrated) |
| `0x07` | `ERR_STORAGE` | EEPROM error |
| `0x08` | `ERR_NOT_READY` | STM32 still initializing (before `HELLO`) |
| `0xFF` | `ERR_INTERNAL` | any other error |

The remaining payload bytes follow only with `OK`, unless stated otherwise below.

### 4.3 Protection against double execution

The STM32 remembers the `seq` and the response frame of the **last** request.
If a request with the same `seq` and the same `msg` arrives again, it is **not
executed again**: the STM32 only resends the stored response. This way, for
example, a repeated `CALIBRATE` does not start a second calibration.

The ESP assigns `seq` sequentially (1…255 wrapping, `0` is skipped). After a
`HELLO` with `reset_seq` set, the STM32 forgets the stored `seq`.

### 4.4 Events

- Events are **not acknowledged**. Lost events do no harm:
  - Every event that carries state contains the **complete** current state of
    its object, never just a delta.
  - In addition the ESP requests `GET_VALVE_STATES` every **10 s**. This is the
    safety net and also serves as a heartbeat.
- The event `seq` lets the ESP detect gaps (counter `evt_lost`). On a gap it
  fetches the complete state immediately instead of waiting 10 s.
- Events are only sent while no response frame is in transmission. A response
  takes priority. Both directions are full duplex.
- Rate limit per channel: at most 10 `EVT_VALVE` per second. For faster
  changes only the latest state is sent.

### 4.5 Startup and handshake

1. After reset the STM32 sends `EVT_BOOT`. The event is repeated every 1 s
   until a `HELLO` arrives.
2. The ESP sends `HELLO`, either in reaction to `EVT_BOOT` or on its own startup.
3. The ESP checks the response:
   - If `proto_version` ≠ 1, no further messages are sent. The status becomes
     `INCOMPATIBLE`, and the web UI offers the STM32 update.
   - `channel_count`, `hw_rev` and `capabilities` are copied into the data model.
4. The ESP transfers the configuration:
   - `SET_SYSTEM_CONFIG`
   - `SET_MOTOR_PARAMS`
   - `SET_CHANNELS`
   - then the targets via `SET_TARGETS`
5. Normal operation follows.

Until the first `HELLO` the STM32 answers every other request with
`ERR_NOT_READY`. It still keeps operating with its last stored configuration
(see 7).

---

## 5. Requests (ESP → STM32)

Message id ranges:

| Range | Content |
|---|---|
| `0x01–0x0F` | system |
| `0x10–0x2F` | valves |
| `0x30–0x3F` | sensors |
| `0x40–0x4F` | diagnostics |
| `0x80–0xBF` | events |

Each entry lists the payload. For responses, the status code from 4.2 is not
repeated.

### 5.1 System

#### `0x01 HELLO`
Request:

| Field | Type | Meaning |
|---|---|---|
| `esp_proto_version` | u8 | protocol version of the ESP |
| `flags` | u8 | bit 0 `reset_seq` |

Response:

| Field | Type | Meaning |
|---|---|---|
| `proto_version` | u8 | `1` |
| `fw_major`, `fw_minor`, `fw_patch` | u8 ×3 | firmware version |
| `fw_build` | u32 | build timestamp (Unix) or CI build number |
| `hw_rev` | u8 | `1` = C1, `2` = C2/C3 |
| `mcu` | u8 | `1` = F401, `2` = F411 |
| `mcu_uid` | u8[12] | 96-bit unique id of the STM32 |
| `channel_count` | u8 | number of channels (12) |
| `capabilities` | u32 | bit 0 DS18B20, bit 1 DS2438, bit 2 RS485, bit 3 CAN |
| `reset_cause` | u8 | `0` unknown, `1` power-on, `2` pin/NRST, `3` software, `4` IWDG, `5` brown-out, `6` low-power |
| `boot_count` | u32 | number of boots (EEPROM) |
| `config_crc` | u16 | CRC of the active system and motor configuration; lets the ESP skip resending it |

#### `0x02 PING`
Request: any 0…16 bytes. Response: the same bytes echoed back. Used to test the link.

#### `0x03 RESET`
Request:

| Field | Type | Meaning |
|---|---|---|
| `magic` | u32 | `0x52535431` ("RST1") |

The STM32 answers `OK` and stops all motors. It writes pending EEPROM data and
then restarts (`NVIC_SystemReset`).

#### `0x04 SET_SYSTEM_CONFIG`

| Field | Type | Range | Default | Meaning |
|---|---|---|---|---|
| `link_timeout_s` | u16 | 0, 10…3600 | 300 | If the STM32 receives no valid request for this long, the ESP is considered failed. `0` = never. |
| `failsafe_pos` | u16 | 0…1000, `0xFFFF` | 1000 | Target position (0.1 %) on ESP failure. `0xFFFF` = hold the current position. |
| `startup_pos` | u16 | 0…1000, `0xFFFF` | `0xFFFF` | Target position after boot until the ESP sends targets. `0xFFFF` = last stored target. |
| `sensor_interval_s` | u16 | 5…3600 | 30 | 1-Wire measurement interval |
| `recal_after_moves` | u16 | 0, 50…10000 | 2000 | Automatic recalibration after this many moves. `0` = off. |
| `recal_after_days` | u8 | 0, 1…60 | 7 | Automatic recalibration after this many days of uptime. `0` = off. |

Stored persistently. Response: `config_crc` (u16).

The defaults apply when the EEPROM holds no valid record. The default
failsafe is **fully open** (`1000`): if the ESP fails, the heating keeps
running and no room can freeze; overheating is the lesser risk. All values
are configurable in the web UI.

#### `0x05 GET_SYSTEM_CONFIG`
Response: the same fields as `SET_SYSTEM_CONFIG`.

### 5.2 Valves

Positions are always given in **0.1 % (0…1000)**: 0 = closed, 1000 = fully
open. Channels are numbered 0 to 11. Channel masks are `u16`; bit n stands for
channel n.

#### `0x10 GET_VALVE_STATES`
Request: empty. Response:

| Field | Type | Meaning |
|---|---|---|
| `count` | u8 | number of channels |
| `count` × `ValveState` | | see 5.4 |

#### `0x11 SET_TARGETS`
Request: `count` (u8, 1…12), then `count` × { `channel` u8, `target` u16 }.

- The targets are accepted and queued for movement.
- If any target is invalid, **none** is accepted: response `ERR_RANGE` or
  `ERR_CHANNEL`, with the index of the faulty entry in the second byte.
- A target equal to the current position is allowed and has no effect.
- If a calibration is running on the channel, the target is stored and
  approached afterwards.

#### `0x12 CALIBRATE`

| Field | Type | Meaning |
|---|---|---|
| `mask` | u16 | channels to calibrate |

The channels are calibrated one after another (only one motor at a time). The
response `OK` only means "scheduled". The result arrives per channel as
`EVT_CALIBRATION`.

#### `0x13 STOP`

| Field | Type | Meaning |
|---|---|---|
| `mask` | u16 | channels to stop |

A running move or calibration is aborted, and pending jobs for these channels
are discarded. Pulses are counted until the motor has come to rest, so the
position stays known; the target is set to the position reached.

#### `0x14 SET_CHANNELS`

| Field | Type | Meaning |
|---|---|---|
| `enabled_mask` | u16 | Only channels whose bit is set are moved, checked and calibrated |

Stored persistently.

#### `0x15 DETECT`

| Field | Type | Meaning |
|---|---|---|
| `mask` | u16 | channels whose connection is checked |

Checks whether a motor is connected with a short test move of 30 pulses
(≈ 0.2 % of a stroke) in opening direction; if no pulse arrives (valve
already fully open), the test is repeated in closing direction. The pulses
are accounted for, so the position does not get lost. The result arrives as
`EVT_VALVE`.

The same detection runs automatically for every enabled channel after boot,
for newly enabled channels and every 10 minutes for channels that are
`NOT_CONNECTED`. A connected channel without calibration data is calibrated
automatically.

#### `0x16 GET_VALVE_DETAIL`
Request: `channel` (u8). Response:

| Field | Type | Meaning |
|---|---|---|
| `channel` | u8 | |
| `state` | `ValveState` | see 5.4 |
| `open_counts` | u32 | pulses for the stroke 0 → 100 % (last calibration) |
| `close_counts` | u32 | pulses for the stroke 100 → 0 % |
| `deadzone_counts` | i32 | `close_counts − open_counts`, signed |
| `calib_age_h` | u32 | hours since the last successful calibration, `0xFFFFFFFF` = never |
| `moves_since_calib` | u32 | |
| `moves_total` | u32 | |
| `mean_current_open_ma10` | u16 | mean current while opening, 0.1 mA |
| `mean_current_close_ma10` | u16 | mean current while closing, 0.1 mA |
| `peak_current_ma10` | u16 | peak current of the last move, 0.1 mA |
| `last_move_counts` | i32 | pulses counted during the last move, signed |
| `last_move_ms` | u32 | duration of the last move |
| `calib_retries` | u8 | retries during the last calibration |
| `last_fault` | u8 | last fault code (see 5.4), kept after the fault is cleared |

#### `0x20 SET_MOTOR_PARAMS` / `0x21 GET_MOTOR_PARAMS`
Shared by all channels. Fields:

| Field | Type | Range | Default | Meaning |
|---|---|---|---|---|
| `endstop_factor_open` | u8 | 11…50 | 17 | endstop detected when the current exceeds `mean × factor/10` (opening) |
| `endstop_factor_close` | u8 | 11…50 | 17 | as above, while closing |
| `endstop_abs_ma` | u16 | 20…300 | 100 | absolute current limit that always triggers an endstop |
| `inrush_blank_ms` | u16 | 50…1000 | 250 | blanking time for the inrush current |
| `opencircuit_ma10` | u16 | 5…100 | 20 | below this current "no motor" is assumed (0.1 mA) … |
| `opencircuit_ms` | u16 | 100…5000 | 500 | … if it persists this long **without interruption** |
| `move_timeout_s` | u16 | 10…600 | 120 | maximum duration of a single move |
| `min_stroke_counts` | u16 | 100…60000 | 3000 | minimum pulses for a valid stroke |
| `max_calib_retries` | u8 | 0…5 | 2 | retries after a failed calibration |

Endstop detection: after the inrush blanking the filtered current is
compared with `mean × factor/10` (capped at `endstop_abs_ma`); 20
consecutive samples (1 kHz) above the threshold stop the motor. Without a
learned mean (first calibration) the mean of the first 300 ms after the
blanking is used. In addition a motor that draws current but delivers no
pulse for 1 s has stalled, which also counts as an endstop.

On `SET` all fields are validated. The first invalid field is reported with
`ERR_RANGE` and its field index (0-based, in table order); nothing is applied
in that case. The values are stored persistently. Response to `SET`:
`config_crc` (u16).

### 5.3 Sensors (1-Wire)

Sensors are always addressed by their **ROM id (u8[8], bus order, byte 0 =
family code)**. List positions have no lasting meaning.

The STM32 searches the bus after boot and on `SENSOR_SCAN`, then measures
every `sensor_interval_s`: one broadcast Convert T for all devices, then
each device is read individually. Supported are DS18B20 (family `0x28`) and
DS2438 (family `0x26`, temperature, VAD and VDD); other families are
ignored. After an error the last valid value is kept, `status` tells what
went wrong and `age_s` keeps growing. At most 32 devices are handled.

#### `0x30 SENSOR_SCAN`
Request: empty. Starts a new bus search. Response `OK` or `ERR_BUSY`. The
result arrives as `EVT_SENSOR_SCAN_DONE`.

#### `0x31 GET_SENSORS`
Request:

| Field | Type | Meaning |
|---|---|---|
| `offset` | u8 | first requested list position |

Response:

| Field | Type | Meaning |
|---|---|---|
| `total` | u8 | total number of sensors |
| `offset` | u8 | as requested |
| `count` | u8 | number of entries in this response |
| `count` × `SensorValue` | | see 5.4 |

At most 12 entries per response (12 × 16 = 192 bytes). With more sensors the
ESP asks again with a higher `offset`.

### 5.4 Shared structures

#### `ValveState` (10 bytes)

| Field | Type | Meaning |
|---|---|---|
| `channel` | u8 | |
| `mode` | u8 | `0` DISABLED, `1` UNKNOWN (not yet checked), `2` NOT_CONNECTED, `3` IDLE, `4` OPENING, `5` CLOSING, `6` CALIBRATING, `7` QUEUED (waiting for the motor driver), `8` FAULT |
| `flags` | u8 | see below |
| `fault` | u8 | current fault, see below |
| `actual` | u16 | actual position, 0.1 %; `0xFFFF` = unknown |
| `target` | u16 | target position, 0.1 % |
| `current_ma10` | u16 | current: live while moving, otherwise mean of the last move (0.1 mA) |

`flags`:

| Bit | Name | Meaning |
|---|---|---|
| 0 | `CALIBRATED` | valid calibration data present |
| 1 | `CALIB_PENDING` | calibration scheduled |
| 2 | `POS_UNCERTAIN` | position not known (no reference run since boot, or lost after a fault) |
| 3 | `AT_ENDSTOP` | endstop detected during the last move |
| 4 | `FAILSAFE` | target comes from `failsafe_pos` |

Bits 5–7 are reserved (0).

`fault`:

| Code | Name | Meaning |
|---|---|---|
| `0` | NONE | no fault |
| `1` | OPEN_CIRCUIT | no current, motor unplugged |
| `2` | BLOCKED | endstop reached more than 15 % before the expected end of stroke, twice in a row (a single occurrence schedules a calibration) |
| `3` | TIMEOUT | `move_timeout_s` exceeded |
| `4` | OVERCURRENT | current above the hard limit of 250 mA for 3 ms (e.g. short circuit), checked from the first sample on |
| `5` | CALIB_FAILED | stroke < `min_stroke_counts` after all retries |
| `6` | NO_PULSES | current flows but not a single back-EMF pulse arrives (detection in both directions, or a move to an intermediate position) |
| `7` | COUNT_MISMATCH | the reference run counted more than the learned closing stroke + 15 %; reported in `last_fault`, a calibration is scheduled |

#### `SensorValue` (16 bytes)

| Field | Type | Meaning |
|---|---|---|
| `rom` | u8[8] | ROM id |
| `kind` | u8 | `1` temperature (DS18B20), `2` voltage VAD (DS2438), `3` voltage VDD (DS2438), `4` temperature (DS2438) |
| `status` | u8 | `0` OK, `1` CRC error, `2` not reachable, `3` not yet measured, `4` out of range (e.g. the 85 °C reset value) |
| `age_s` | u16 | seconds since the last valid reading, capped at `0xFFFF` |
| `value` | i32 | milli-units: m°C or mV |

A DS2438 shows up as up to three entries with the same ROM id and different `kind`.

### 5.5 Diagnostics

#### `0x40 GET_DIAG`
Request: empty. Response:

| Field | Type | Meaning |
|---|---|---|
| `uptime_s` | u32 | |
| `rx_frames_ok` | u32 | |
| `rx_crc_err` | u16 | |
| `rx_malformed` | u16 | COBS error, bad length, unknown `kind` or `seq` = 0 |
| `rx_overflow` | u16 | |
| `rx_bad_version` | u16 | |
| `rx_duplicate` | u16 | repetitions answered via 4.3 |
| `uart_hw_err` | u16 | framing or overrun errors of the UART hardware (not implemented yet: 0) |
| `ow_crc_err` | u16 | |
| `ow_bus_err` | u16 | bus short circuit, no presence pulse |
| `eeprom_writes` | u32 | |
| `eeprom_err` | u16 | |
| `loop_max_us` | u32 | longest main loop pass since boot |
| `mcu_temp_c10` | i16 | internal STM32 temperature, 0.1 °C (not implemented yet: 0) |

#### `0x41 CLEAR_DIAG`
Request: empty. Resets the counters; uptime and the EEPROM counters are kept.

---

## 6. Events (STM32 → ESP)

| Id | Name | Payload | When |
|---|---|---|---|
| `0x80` | `EVT_BOOT` | like the `HELLO` response without status code | after reset, repeated until `HELLO` |
| `0x81` | `EVT_VALVE` | `ValveState` | on every change of `mode`, `flags` or `fault`; at the end of a move; during a move at most every 500 ms with the current position |
| `0x82` | `EVT_CALIBRATION` | `channel` u8, `result` u8 (`0` OK, otherwise a `fault` code), `open_counts` u32, `close_counts` u32, `retries` u8 | end of a calibration |
| `0x83` | `EVT_SENSORS` | like the `GET_SENSORS` response without status code | after every measurement cycle; several events if > 12 sensors |
| `0x84` | `EVT_SENSOR_SCAN_DONE` | `total` u8, `ds18_count` u8, `ds2438_count` u8, `bus_ok` u8 | end of `SENSOR_SCAN` |
| `0x85` | `EVT_LINK_TIMEOUT` | `failsafe_pos` u16 | `link_timeout_s` expired; sent as soon as the ESP responds again |
| `0x86` | `EVT_LOG` | `level` u8 (`0` error, `1` warn, `2` info, `3` debug), `text` (UTF-8, ≤ 200 bytes, not null-terminated) | diagnostic messages; the ESP forwards them to syslog or the web UI |

`EVT_LOG` with level debug is only sent if the ESP enables it via
`SET_SYSTEM_CONFIG`. An extra field for that is planned for v1.1; v1 only sends
error, warn and info. This removes the need for the STM32 debug UART in normal
operation.

---

## 7. STM32 behaviour without the ESP

- The STM32 keeps operating with its last stored configuration (system, motor,
  channels) even without the ESP.
- After boot:
  1. Every enabled channel is checked with a detection move (see `DETECT`).
  2. A connected channel with valid calibration performs a **reference run**
     towards closed until the endstop; one without calibration is calibrated.
  3. Then it moves to its target: `startup_pos`, or the last stored target
     if `startup_pos` = `0xFFFF` (targets are stored at most every 10 minutes;
     without any stored target the failsafe position is used).
  4. If the reference run counts more pulses than the learned closing stroke
     + 15 %, the STM32 reports `COUNT_MISMATCH` in `last_fault` and schedules
     a calibration.
- Only one motor moves at a time; channels with pending work report
  `QUEUED`. The valve power supply is switched on 1 s before the first move
  and off 10 s after the last one.
- If `link_timeout_s` ≠ 0 and no valid request arrives for that long:
  - If `failsafe_pos` ≠ `0xFFFF`, every channel is moved there, with the
    `FAILSAFE` flag set.
  - The first valid request afterwards ends the failsafe state. The targets
    stay unchanged until the ESP sends new ones.
  - Failsafe targets are never stored in the EEPROM.

---

## 8. Example

The ESP sets channel 3 to 45.0 %:

```
REQ  ver=1 kind=1 seq=17 msg=0x11  payload: 01 03 C2 01        (count=1, ch=3, target=450)
RSP  ver=1 kind=2 seq=17 msg=0x11  payload: 00                 (OK)
EVT  ver=1 kind=3 seq=88 msg=0x81  payload: 03 04 01 00 2C 01 C2 01 D2 00
                                   (ch=3, OPENING, CALIBRATED, actual=300, target=450, 21.0 mA)
EVT  ver=1 kind=3 seq=89 msg=0x81  payload: 03 03 01 00 C1 01 C2 01 CA 00
                                   (ch=3, IDLE, actual=449, target=450, 20.2 mA)
```

Each decoded frame is written as `ver kind seq msg payload… crc_lo crc_hi`. It
is COBS encoded and terminated by `00` on the wire.

---

## 9. Bandwidth (sanity check)

115200 baud ≈ 11.5 kB/s.

- Base load: `GET_VALVE_STATES` every 10 s, about 140 bytes in both directions.
- Sensor events every 30 s with 30 sensors, about 500 bytes.
- `EVT_VALVE` every 500 ms during a move, about 20 bytes each.

Together this is **well below 1 %** of the capacity. There is no need to raise
the baud rate.

---

## 10. Implementation notes

- Shared C99 code in `common/protocol/` without Arduino dependency. Both
  firmwares include it; it is tested on the host (`env:native`).
- Encoder and decoder per message as a pair of functions
  (`vdm_enc_set_targets()`, `vdm_dec_set_targets()`). The decoder validates
  length and ranges, not the caller.
- No dynamic memory on the STM32, and no `String`s in the link path on the ESP32.
- The examples of chapter 8 are part of the host tests
  (`common/protocol/test/`), so the specification and the code cannot drift
  apart unnoticed.

---

## 11. Open points

- [ ] Baud rate change (`SET_LINK`) – probably unnecessary, see 9.
- [ ] Enabling `EVT_LOG` debug (v1.1).
- [ ] RS485/CAN: unused in v1; a later extension gets its own id range `0x50–0x5F`.
