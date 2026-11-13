# Project conventions

## Language
- Everything that goes into the repository is written in **English**: source
  code, identifiers, code comments, documentation, commit messages, PR
  descriptions, issues and release notes. This is a public repository.
- Conversation with the maintainer may be in German.

## Context
- The firmware is being rebuilt as v2. Read `docs/refactoring/PLAN.md` for the
  decisions and phases, and `docs/protocol/stm32-esp32-link.md` for the
  ESP32 ⇄ STM32 protocol.
- `software_stm32/` and `software_esp32/` are the legacy v1.4.x code. Do not
  modify them; new code goes into `common/` and `firmware/`.
- License is GPLv3. Keep upstream copyright notices in any code taken over
  from the legacy firmware.

## Build and test
- Protocol host tests: `cd common/protocol && pio test -e native`
- STM32 firmware: `cd firmware/stm32 && pio run` (envs `f411_c2`, `f401_c2`)
- ESP32 firmware: `cd firmware/esp32 && pio run` (env `wt32_eth01`)
- Formatting: `clang-format` with the repository's `.clang-format`, applied to
  `common/` and `firmware/` only; CI checks it.
