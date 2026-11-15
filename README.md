# VdMot Controller – firmware v2

A controller for up to 12 **HmIP-VdMot** valve actuators (or other DC motor
valves) for floor heating and radiators – local, without a cloud, with its
own room control and a Home Assistant integration.

This repository is a fork of the VdMot Controller by
[Lenti84](https://github.com/Lenti84/VdMot_Controller) and
[SurfGargano](https://github.com/SurfGargano/VdMot_Controller), who designed
the hardware and wrote the firmware 1.x. The firmware of both controllers
was rebuilt here as **v2**.

![C2 sample PCB with soldered components](./hardware/c2-sample_pcb_soldered.jpg)

## Features of firmware v2

- **Valves**: learns every valve (opening and closing stroke counted by the
  motor's back-EMF pulses), endstop detection by current, calibration kept
  over restarts, automatic recalibration, faults detected and reported
  (open circuit, blocked, timeout, …), valves re-detected when plugged in.
- **Room control on the controller**: zones (rooms or heating circuits)
  with one or more valves and a PI controller (floor / radiator / custom
  profile); temperatures from 1-Wire sensors or MQTT, several sensors per
  zone averaged.
- **Heating and cooling**: what the water supplies is judged per zone from
  the supply temperature; the controller pauses while a cycling heat pump
  is off (no wind-up), cools with its own setpoint.
- **Safety**: minimum total opening for systems without a buffer tank or
  bypass, fallback position when a temperature is missing, and a failsafe
  on the STM32 that opens the valves when the ESP32 is gone.
- **Home Assistant** via MQTT discovery: one device per zone with a climate
  (heat/cool with both setpoints), valve positions and targets, states and
  faults; a blueprint sends Home Assistant temperatures to the controller.
- **Web page** for setup and diagnostics; configuration export/import;
  updates of both controllers through the web page (the ESP32 flashes the
  STM32), no USB adapter needed.
- **Robust link** between ESP32 and STM32: binary protocol with framing,
  CRC and retries.

## Installation

Binaries: [latest release](https://github.com/apoapoapo/VdMot_Controller/releases/latest).
Installing and upgrading from the firmware 1.4.x, step by step:
[docs/installation.md](./docs/installation.md). Going back to 1.4.x is
possible at any time.

## Documentation

| Document | Content |
|---|---|
| [docs/installation.md](./docs/installation.md) | installation, upgrade, first steps, room control, Home Assistant |
| [docs/design/esp32-control-and-integration.md](./docs/design/esp32-control-and-integration.md) | configuration, room control, heating/cooling, flow guard, MQTT topics, Home Assistant entities |
| [docs/protocol/stm32-esp32-link.md](./docs/protocol/stm32-esp32-link.md) | protocol between ESP32 and STM32 |
| [docs/refactoring/PLAN.md](./docs/refactoring/PLAN.md) | why and how v2 was built, open points |
| [hardware/hardware.md](./hardware/hardware.md) | hardware |

## Repository layout

- `firmware/esp32`, `firmware/stm32`, `common/protocol`: firmware v2
  (PlatformIO; host tests with `pio test -e native`)
- `blueprints/`: Home Assistant blueprint
- `hardware/`, `assembly_kit/`, `system/`: hardware of the original project
- `software_esp32/`, `software_stm32/`: legacy firmware 1.4.x, unchanged
  (described in [software.md](./software.md))

## Hardware

The hardware (C2/C3 board with an STM32 BlackPill and a WT32-ETH01) comes
from the original project; see [hardware/hardware.md](./hardware/hardware.md).
Hardware was available via the original authors, see their repositories.

## License

GNU General Public License v3.0, see [GPLv3-LICENSE.md](./GPLv3-LICENSE.md).
Copyright of the original hardware and firmware 1.x: Lenti84 and
SurfGargano.
