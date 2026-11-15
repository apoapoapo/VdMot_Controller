# VdMot Controller – Firmware rebuild (v2)

Date: 2026-11-13 · Status: phases 0–3 done, release 2.0.0

This document records **why** the firmware is being rebuilt, **which decisions**
have been made and **in which order** the work is done. The UART protocol is
specified in [`../protocol/stm32-esp32-link.md`](../protocol/stm32-esp32-link.md).

---

## 1. Starting point

The hardware (C2 revision, STM32F411 "BlackPill" WeAct V3.0, WT32-ETH01) is good.
The existing firmware (v1.4.x) has structural problems that cannot be fixed with
isolated patches. The most important ones (excerpt from the code review):

| Area | Problem | Effect |
|---|---|---|
| STM32 motor | Command (`valvenr`, `command`, `poschangecmd`) is handed from main loop to the 10 ms ISR without protection | wrong valve / wrong travel, drift |
| STM32 motor | Position is only accumulated nominally, counted pulses are never fed back; `scaler` is an integer; `deadzone` is unused and `unsigned` (underflow) | position drift |
| STM32 motor | Undercurrent counter is cumulative instead of consecutive | valve is flagged "open circuit" and disappears |
| STM32 motor | `delay(100)` in the ISR path (MUX switching) | current measurement stalls, UART bytes are lost |
| STM32 | Calibration/position not persisted, EEPROM without version/CRC, no watchdog, no BOR | all valves re-learn after every boot; hangs never recover |
| STM32 1-Wire | Bus index and DS18 index are mixed up, blocking ROM searches | temperatures assigned to the wrong valve |
| UART protocol | Text based, no framing/CRC/sequence number, buffer overflows on both sides, only the first line in the buffer is processed, `gvlst` parser increments the index twice | lost or misassigned responses |
| ESP32 | AsyncWebServer callbacks write global state without locks, `yieldTask()` runs the scheduler on the wrong thread, all watchdogs disabled | sporadic faults, hangs |
| ESP32 config | NVS struct blobs without version/CRC | any layout change corrupts the configuration |
| ESP32 MQTT/HA | No LWT/availability, client id = station name, actual position never published, discovery entities without data or without subscription, discovery blocks for ~20 s | unreliable Home Assistant integration |
| ESP32 security | No endpoint is actually protected (reboot, OTA, config), SMTP password sent to the UI in clear text | – |
| PI controller | Δt taken from `time()` (SNTP jump), start position set from a temperature instead of a percentage | integrator kicks |

An incremental refactoring would touch almost every line. Therefore:
**rebuild following the strangler pattern** – hardware knowledge and the useful
algorithms (current-based endstop detection, back-EMF pulse counting, basic PI
structure, STM32 update via ROM bootloader) are kept, the structure is not.

---

## 2. Decisions

| # | Decision | Rationale |
|---|---|---|
| D1 | Rebuild in the **own fork** (`apoapoapo/VdMot_Controller`); new firmware lives in new directories; the old code stays untouched until it is replaced | history and authorship stay visible, no big bang |
| D2 | License stays **GPLv3**; existing copyright notices are kept in any reused code, new files carry their own header plus the GPLv3 notice | required by the GPL for derived works |
| D3 | Own versioning starting at **2.0.0** (SemVer), own GitHub releases in the fork; README credits upstream (Lenti84 / SurfGargano) | clear separation from the upstream 1.x releases |
| D4 | STM32 = pure real-time actuator, ESP32 = logic/configuration/integration | one source of truth per concern |
| D5 | New **binary UART protocol** (COBS + CRC16, sequence numbers, request/response + events), shared header for both sides | see protocol specification |
| D6 | STM32 and ESP32 are always updated **together** (ESP flashes the STM via ROM bootloader); no compatibility with the 1.x text protocol | avoids transition code |
| D7 | Integration: **MQTT / Home Assistant first**; KNX will be discussed separately later as its own adapter | HA is almost there, fastest benefit |
| D8 | Primary target hardware **C2 + STM32F411**; F401 and C1 remain possible via build environments but are not primarily tested | hardware at hand |
| D10 | On ESP failure the STM32 opens **all valves fully** after `link_timeout_s` (default 300 s) | heating keeps working, no frost risk; overheating is the lesser risk |
| D11 | Automatic recalibration defaults stay as in 1.x (2000 moves / 7 days); both values and the failsafe behaviour are configurable in the web UI | no field experience yet with other values |
| D9 | Code, comments, documentation, commit messages and issues are written in **English** | public repository |

Open / later:

- **KNX**: approach still open (KNX-IP from the ESP32 without an ETS product
  database vs. a TP-UART transceiver with an ETS application, e.g. based on the
  OpenKNX tooling). The earlier prototype (TP converter on the STM debug UART)
  will not be continued.
- **Additional 3.3 µF / 50 V capacitor on the ESP connector** (present on the
  owner's board, purpose unknown). Most likely either a bulk capacitor for the
  WT32-ETH01 supply (current spikes from WiFi/PHY → brown-out resets) or an RC
  delay on the EN pin (known WT32-ETH01 boot issue, because GPIO0 is driven by
  the LAN8720 clock). **Find out which pins it is connected to** and document it
  in the hardware documentation.

---

## 3. Target repository layout

```
common/
  protocol/            shared protocol library (C99, no Arduino dependency)
    include/vdm/link.h   message ids, structures, error codes, defaults
    include/vdm/crc16.h  CRC-16/CCITT-FALSE
    include/vdm/cobs.h   COBS encode/decode
    include/vdm/frame.h  frame builder and stream receiver
    include/vdm/codec.h  message (de)serialization, little-endian, field by field
    test/                Unity host tests (pio test -e native)
firmware/
  stm32/               new STM32 firmware (PlatformIO)
    src/hal/           pins, timers, ADC, pulse counter, EEPROM, 1-Wire (only place using Arduino/HAL)
    src/motor/         MotorChannel (state machine), ValveManager, calibration – hardware independent
    src/sensors/       non-blocking 1-Wire scheduler, sensor list keyed by ROM id
    src/link/          protocol server
    src/storage/       EEPROM records with version + CRC
    test/              Unity tests (env:native)
  esp32/               new ESP32 firmware (PlatformIO)
    src/core/          data model (mutex protected), event bus
    src/link/          protocol client, STM32 update via ROM bootloader
    src/config/        versioned configuration (JSON on LittleFS) + migrations
    src/control/       PI controller (monotonic time base)
    src/adapters/      web (REST + WebSocket), mqtt_ha, later knx
    test/
  webui/               web UI with a reproducible build (no mktfs.exe)
docs/
  protocol/            protocol specification
  refactoring/         this document
software_stm32/, software_esp32/   legacy code (v1.4.x), removed once replaced
```

Release binaries (`releases/`, `*.bin`) do not belong in the repository; they
are attached to GitHub releases (built by CI).

---

## 4. Phases

### Phase 0 – Foundation (done)
- Directory layout, `.gitignore`, `.editorconfig`, clang-format.
- Protocol specification (this package) → review → implement `common/protocol`.
- Host tests (PlatformIO `env:native` + Unity) for COBS, CRC, codec.
- GitHub Actions: builds for STM32 (F411/F401) and ESP32, host tests.
- Firmware skeletons (`firmware/stm32`, `firmware/esp32`) that link the
  protocol library: the STM32 puts all motor outputs into a safe state and
  answers `HELLO`/`PING`; the ESP32 sends `HELLO` once per second and prints
  the answer. Usable as a first link test on the hardware.
- Legacy release binaries removed from the repository (they remain in the
  git history and in the upstream releases).

### Phase 1 – STM32 firmware (most critical part)

Done:
- `src/core` (hardware independent, host tested):
  - `MotorRun`: one movement in interrupt context – current filter,
    inrush blanking, endstop by current (learned or measured reference,
    capped by the absolute limit) or by stall, open circuit (consecutive
    samples), hard overcurrent limit, timeout; stops the driver directly in
    the interrupt and counts coasting pulses.
  - `ValveManager`: 12 channels, one motor at a time, PSU and MUX settle
    times, jobs detect → calibrate → reference → move, position from
    counted pulses with separate opening/closing strokes, endstop moves
    re-reference, unexpected endstop → recalibration / `BLOCKED`, fault
    retry every 6 h, re-detection every 10 min, recalibration after
    moves/days.
  - `Storage`: EEPROM records with A/B slots, sequence number and CRC,
    non-blocking page writes, power-loss safe.
  - `LinkServer` + `Controller`: all protocol v1 requests, duplicate
    protection, events, boot handshake, failsafe, lazy persistence.
  - `SensorManager`: non-blocking 1-Wire measurement (at most ~14 ms bus
    time per main-loop pass), ROM search, broadcast Convert T, DS18B20 and
    DS2438 (temperature, VAD, VDD), CRC checks, last value kept on errors.
- `src/board`: pins (C1/C2), motor driver, 1 kHz sampling (TIM11) and pulse
  interrupt at equal priority, 24LC64 EEPROM, bit-banged 1-Wire master on
  PB10, USART1 with 512-byte buffers, IWDG (2 s), reset cause, MCU UID.
- 57 host tests with a valve simulator (asymmetric strokes, coasting,
  obstruction, disconnected motor, missing pulses, endless motor) and a
  bit-level 1-Wire bus simulator, including 150 random moves without drift
  and end-to-end protocol tests.

Open:
- DS2482 1-Wire bridge (optional on the board), not supported yet.
- BOR level (option bytes), UART error counters, MCU temperature.
- **Hardware test on the owner's board** (flash via the ESP32 once phase 2
  provides the update path, or via USB DFU / ST-Link).

### Phase 2 – ESP32 core

Done (2a, bring-up for the first hardware test):
- `src/core` (host tested): `LinkClient` (handshake, stop-and-wait with
  retries, polling, events, gap detection, user targets resent after an
  STM32 restart, hard-reset request when the STM32 stays silent) and
  `Stm32Flasher` (AN3155: bootloader entry via the legacy-compatible
  "DEADBEEF" handshake – BOOT0 is not wired on C2/C3 – with BOOT0 as
  fallback, chip id, sector erase, 256-byte writes with retry, read-back
  verification).
- Integration test: `LinkClient` against the real STM32 core with valve,
  1-Wire and EEPROM simulators over a wire that drops and corrupts bytes.
- `src/app`: link task (UART, LinkClient, STM32 update from LittleFS),
  network (Ethernet, WiFi fallback, setup AP; settings read from the
  legacy NVS without modifying it), web page + REST API (status, targets,
  calibration, settings, STM32 flashing, ESP32 OTA, start previous ESP32
  firmware), basic auth with the legacy web login, task watchdog.
- Installation guide: [`../installation.md`](../installation.md).

Done (2b), see the design [`../design/esp32-control-and-integration.md`](../design/esp32-control-and-integration.md):
- `src/core` (host tested): `Config` (JSON, validation, sections with id
  assignment and reference clean-up), `ConfigStore` (atomic write with
  temporary file and backup), `ControlEngine` (temperature sources with
  stale detection and start-up grace, zones in mode control/manual, PI
  controller with conditional integration, bumpless start and RTC
  snapshot, valve limits and factor, flow guard with priority valves),
  tests with a room model in closed loop.
- `src/app`: settings owner (`/config.json`, network settings taken over
  once from the legacy NVS), control loop (targets to the STM32, STM32
  failsafe position = fallback position, controller state in RTC RAM,
  log), network from the configuration, web page with tabs (overview,
  rooms & valves, settings, system) and configuration export/import.

Also done in 2b (from the first hardware tests): heating and cooling from
the supply temperature with a pause while nothing is supplied (zones
independent), several control sources per zone (mean), calibration ending
fully open (STM32).

Open: syslog.

### Phase 3 – MQTT / Home Assistant (done)
- PubSubClient in its own task, LWT `<base>/status`, client id derived
  from the MAC (configurable), states on change and every 5 minutes,
  commands for zones, valves and global settings; MQTT temperature
  sources.
- Home Assistant discovery: controller device and one device per zone
  (climate `heat_cool`/`off` with both setpoints, valve target sliders
  that are unavailable while the zone controls, positions, states,
  faults, further temperatures); `unique_id`s from the client id, entities
  of removed zones/sources deleted; checked against the discovery
  schemas of Home Assistant.
- Blueprint `blueprints/automation/vdmot/send_temperature.yaml` to send
  Home Assistant temperature sensors to MQTT sources.

### Phase 4 – Web UI
- New, small, built against the REST/WebSocket API; build step integrated into PlatformIO.

### Phase 5 – KNX (after a separate discussion)
- As an adapter on the data model (DPT 5.001 valve position, DPT 9.001
  temperature, status/fault).

---

## 5. Test and acceptance criteria (excerpt)

- 72 h endurance run with random targets: position error after a reference run < 2 %.
- STM32 restart: no re-learn, only a reference run; positions correct.
- Unplugging/replugging a valve: detected, fault reported, operation resumes.
- Disturbed UART (dropped/corrupted bytes): no wrong action, resync < 1 s.
- ESP32 restart: valves keep their position, HA briefly shows "unavailable".
- Home Assistant: every entity has data, every command takes effect.
