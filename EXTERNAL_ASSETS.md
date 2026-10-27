# External assets for Analog Mimic

Assets required to build, flash, or understand this project that are
NOT covered by this repository's licenses. See LICENSE.md for scope.

## Documents (not redistributed)

### PY32F071-E Datasheet

- Source: https://www.puyasemi.com/en/download.html
- Revision: V1.4
- Retrieved: 2026-04-30
- File: PY32F071-E_Datasheet_V1.4.pdf
- SHA-256: d5384208f3c15d1defbb4a4491cd8eb80cf81f94
- License: © Puya Semiconductor. Not redistributed in this repository.

### PY32F071-E Reference Manual

- Source: https://www.puyasemi.com/en/download.html
- Revision: V0.2
- Retrieved: 2026-04-30
- File: PY32F07X-E_Reference_Manual_V0.2.pdf
- SHA-256: 61a883298e8efe4832f9e3781d17a7837e0bd95a
- License: © Puya Semiconductor. Not redistributed in this repository.

## Firmware dependencies

### PY32F071 Firmware Driver Library  (git submodule)

- Upstream: https://github.com/OpenPuya/PY32F071_Firmware (branch: master)
- Pinned commit: 73e384cddce63e3019de41d9b6af17dfc96fa536
- Retrieved: 2026-04-30
- Local modifications: none
- Mirror (fallback): https://github.com/tkgktyk/PY32F071_Firmware
- License: BSD-3-Clause
  © 2019 Puya Semiconductor / © 2024 OpenPuya
  Full text: THIRD_PARTY_NOTICES.md
- Note: pinned deliberately; characterization data in the paper refer to
  binaries built against this commit.

## Toolchain (reference build environment)

### arm-none-eabi-gcc
- Arm GNU Toolchain 15.2.Rel1 (Build arm-15.86), 15.2.1 20251203
- Source: https://gitlab.arm.com/tooling/gnu-toolchains-for-arm/-/tree/releases/15.2.rel1?ref_type=heads
- File: arm-gnu-toolchain-15.2.rel1-mingw-w64-x86_64-aarch64-none-elf.msi
- SHA-256: 1b8fa008f4a243e5701133f55d9e07616592feeac7dcb3079fc1c59b98fc42eb
- Host OS: Windoes 11 Home 25H2

### GNU make

- Version: 4.4.1
- Built for x86_64-w64-mingw32

## Flashing

### pyOCD

- ```pip install pyocd==0.43.1```
- Version 0.43.1

### Puya PY32F0 Series Device Support (CMSIS DFP)
- Install: `pyocd pack install py32f071xb`
- Version: 1.2.8 (released 2025-07-28)
- Retrieved: 2026-04-30
- License: see .pdsc in the pack
- Note: `pack install` does not accept a version. To pin explicitly,
  obtain the .pack file and pass `--pack <file>` (or set `pack:` in
  pyocd.yaml).

### Puya PY32F0 Series Device Support

- Source: https://pypi.org/project/pyocd/0.43.1/
- Install: ```pyocd pack install py32f071xb```
- Version 1.2.8
- July 28, 2025

## Hardware design

### KiCAD

- Source: https://www.kicad.org/download/
- Version: 10.0.3
