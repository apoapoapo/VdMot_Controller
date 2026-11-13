# Hardware bring-up of firmware v2

This guide describes the first test of firmware v2 on a VdMot Controller
board (C2/C3, WT32-ETH01, STM32F411 or STM32F401 BlackPill) **without a USB
adapter**: everything is done through the web interfaces.

## 1. Get the binaries

The CI builds them for every pull request. Open the pull request, go to
**Checks → CI → Summary** and download the binaries (single `.bin` files,
named after target, board and build number, e.g.
`vdmot-v2-stm32-f401ce_c2-build12.bin`):

| Binary | Use |
|---|---|
| `vdmot-v2-esp32-wt32_eth01-buildN.bin` | ESP32 (WT32-ETH01) |
| `vdmot-v2-stm32-f411_c2-buildN.bin` | STM32**F411**CE BlackPill |
| `vdmot-v2-stm32-f401_c2-buildN.bin` | STM32**F401**CC BlackPill (V2.0); also runs on the F401CE |
| `vdmot-v2-stm32-f401ce_c2-buildN.bin` | STM32**F401**CE BlackPill (WeAct V3.0) |

Check the marking on the BlackPill's chip (`STM32F411CEU6`, `STM32F401CCU6`
or `STM32F401CEU6`; WeAct V3.0 boards exist with F401CE and F411CE). Every
v2 STM32 image names the chips it is built for; the ESP32 compares this
with the chip the bootloader reports and refuses a wrong image before
erasing anything.

Keep the legacy binaries at hand to go back (upstream releases of
VdMot_Controller 1.4.x: `ESP32_firmware.bin`, `STM32F411_C2_firmware.bin` /
`STM32_C2_firmware.bin`).

## 2. Update the ESP32 first

1. Open the legacy web interface → update page → **ESP32 update** and upload
   the v2 `firmware.bin` of the ESP32.
2. The ESP32 restarts with v2. On its very first start it takes over the
   legacy network settings (Ethernet/WiFi, DHCP or static IP, station name
   and web login) into its own configuration (`/config.json`), so it comes
   up at the same address. The legacy NVS is not changed.
3. If it cannot be reached after about a minute: v2 opens a WiFi access
   point **`VdMot-XXXX`**, password **`vdmot-setup`**; the page is at
   `http://192.168.4.1/`.

The page shows the STM32 link as **disconnected**: the legacy STM32 firmware
does not speak the new protocol. In this state the ESP32 resets the STM32
every 5 minutes (it assumes a hang), which does no harm.

## 3. Flash the STM32

1. Section **Firmware** → STM32 image → choose the v2 STM32 `firmware.bin`
   for your chip → **Flash STM32**.
2. Progress: connecting → erasing → writing → verifying → done. The result
   line shows the detected chip. The ESP32 resets the STM32 and asks the
   running firmware (legacy 1.x or v2) to jump to the ROM bootloader
   ("DEADBEEF" handshake) – BOOT0 is not wired on the C2/C3 boards.
   The reset only reaches the STM32 if jumper **X20 ("stm32 reset")** is
   set and the reset circuit is populated. Otherwise the page asks you to
   **press the RESET button on the BlackPill** – do so within 30 s. After
   writing, the new firmware is started by the bootloader itself (GO
   command), no reset needed. The line *Bootloader entry* shows what
   happened in detail.
3. **Recovery** – if the STM32 has no working firmware any more (wrong
   image, interrupted update; the LED does not blink after RESET), the
   handshake cannot work. Put the STM32 into its ROM bootloader by hand:
   hold **BOOT0**, press and release **RESET** (or plug in the power),
   release BOOT0. The STM32 now waits in the bootloader (no LED activity).
   Then click **Flash STM32**: the ESP32 finds the active bootloader in its
   first step (*Bootloader entry: probe: bootloader active*).
4. The link changes to **connected**, the header shows both versions.

## 4. What happens next (automatically)

- **Detection**: every enabled channel makes a tiny opening move
  (30 pulses ≈ 0.2 %) to check whether a valve is connected. Empty channels
  become *not connected* after about one second each.
- **Calibration**: connected channels without calibration data are learned
  one after another: close to the endstop, open to the endstop (count),
  close again (count). One stroke takes roughly a minute.
- **Target**: on the very first start there is no stored target, so the
  valves go to the failsafe position **100 % (open)**.
- After the first calibration a **power cycle must not trigger a new
  calibration** – only a reference run (close to the endstop) per valve.
- **Failsafe**: if the STM32 hears nothing from the ESP32 for 5 minutes
  (e.g. the ESP32 is being updated or is back on the legacy firmware), it
  opens all valves. To test it without unplugging anything: set a short
  *Link timeout* (e.g. 30 s) in the STM32 settings, then use *Diagnostics →
  Pause link*. The valves open after the timeout; when the pause ends the
  log shows "STM32 was in failsafe" and the valves return to their targets.

## 5. What to check and report

- Link: state stays *connected*; Diagnostics → `retries`, `timeouts`,
  `stm_rx_crc_err` should stay at or near 0.
- Per valve: calibration finishes (state *idle*, flag `cal`), the learned
  strokes (open/close pulses) are plausible and similar between valves of
  the same type, no *fault*.
- Moves: set a few targets (e.g. 20 %, 70 %, 45 %, 0 %, 100 %). Does the
  valve move, and does the reported position match? Moves to 0 % and 100 %
  run into the endstop.
- Currents (mA column) while moving and the endstop detection: does it stop
  at the endstop promptly? Faults like *timeout* or *no pulses* point to
  wrong thresholds – the motor parameters can be changed on the page.
- Sensors: all 1-Wire sensors listed with plausible values.
- Power cycle: positions are re-established by reference runs, no
  recalibration.

Screenshots of the page and the values of the Diagnostics section help most.

## 6. Room control (phase 2b)

After the update all valves are without a zone and keep their positions;
nothing changes until zones are set up.

1. **Rooms & valves → Temperature sources:** the 1-Wire sensors found on the
   bus are listed below the table; *add* the ones you want, give them names
   and **Save sources**.
2. **Zones:** *Add zone*, name it, choose the control temperature and the
   profile (floor / radiator), **Save zones**.
3. **Valves:** assign the valves to their zone, optionally minimum opening,
   factor and flow priority, **Save valves**.
4. **Overview:** per zone temperature, setpoint, output and state. Change
   the setpoint; within the sample time (60 s radiator, 300 s floor) the
   output and the valve targets follow. The column *Requested → effective*
   shows when limits or the flow guard change a target.
5. **Settings → Flow guard:** set a minimum total opening (e.g. 150) and
   check that the flow guard opens the priority valves first when all zones
   are satisfied (heating off on all zones is the quickest test).

What to check and report:
- Mode *manual*: the valve target inputs become editable, the setpoint is
  locked; back to *control* the output continues without a jump.
- Temperature missing: unplug a 1-Wire sensor of a zone; after its max age
  (300 s) the zone shows *fallback* and the valves go to the fallback
  position; plug it in again and control resumes. The log shows both.
- ESP32 restart: the zones keep their output (controller state in RTC RAM),
  the valves do not move.
- Settings → Device / Network: change something, restart, check it is kept.
  **Export configuration** once and keep the file.

## 7. Going back to the legacy firmware

1. v2 page → **Start previous ESP32 firmware**: the ESP32 restarts with
   the firmware still stored in its other OTA slot (the legacy 1.4.x).
2. Legacy web interface → STM32 update → upload the legacy STM32 binary.

Alternatively upload the legacy `ESP32_firmware.bin` through the v2 ESP32
update.

## Known limitations of this test version

- No MQTT / Home Assistant yet (phase 3): MQTT settings and MQTT
  temperature sources can be configured but are not used yet.
- The current measurement scale (138 µA per ADC digit) and the default
  thresholds are taken from the legacy firmware.
