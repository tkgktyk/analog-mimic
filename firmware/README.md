# firmware/README.md

This directory contains the core firmware source code, peripheral drivers, and build configurations for the Analog Mimic module. This guide outlines the setup required for compiling the firmware from source and flashing the compiled binary onto the target microcontroller.

## 1. Development Environment Setup

Analog Mimic firmware is targeted for the Puya Semiconductor PY32F071 MCU (Arm Cortex-M0+, 72 MHz). To compile and flash the firmware, the following toolchains and utility tools are required.

### Required Software Components

1. **GCC ARM Embedded Toolchain (`arm-none-eabi-gcc`)**
* Used for compiling and linking the C source code into an ARM architecture binary.
* Ensure it is added to your system environment variable `PATH`, or pass `GCC_PATH` to `make`.

2. **GNU Make**
* Executes the compilation pipeline via the `Makefile`.

3. **Python 3.x & pyOCD**
* pyOCD is an open-source Python library used for programming and debugging ARM Cortex microcontrollers via a CMSIS-DAP debugger.

### Driver Library (git submodule)

The PY32F071 driver library (`firmware/lib/PY32F071_Firmware`) is a git submodule. Clone the repository with `--recursive`, or fetch it after cloning:

```bash
git submodule update --init
```

---

## 2. Compilation and Build Pipeline

All build commands are issued from `firmware/targets/mimic0x/`. The firmware is compiled with `-O2` and link-time optimization to meet the 360-clock-cycle budget of the 200 kHz primary ISR, and `.ramfunc` places time-critical code in zero-wait-state SRAM.

### Build Commands

* **Compile firmware for the default board (mimic01):**
```bash
make
```

* **Compile firmware for all boards (mimic00 and mimic01, after a clean):**
```bash
make release
```

*Each board is built in its own directory, e.g. `build/MIMIC01/analog_mimic_MIMIC01.elf`, `.bin` and `.hex`.*

* **Clean Build Artifacts:**
```bash
make clean
```

---

## 3. Flashing & Debug Environment

Target programming is handled via the SWD (Serial Wire Debug) pins using a standard CMSIS-DAP compliant debug probe.

### Target MCU Specification

* **Target Device:** Puya PY32F071xB
* **Memory Config:** 128 KB Flash / 16 KB SRAM

### Flash Programming via pyOCD

pyOCD supports the PY32 series by loading the corresponding device pack.

```bash
pyocd pack install py32f071xb
```

Flash the compiled Intel HEX image (from `firmware/targets/mimic0x/`):

```bash
pyocd flash -t py32f071xb build/MIMIC01/analog_mimic_MIMIC01.hex
```

### Hardware Connection

```
[ Host PC ]
    │ (USB)
    ▼
[ CMSIS-DAP Debugger ] (e.g., DAPLink, J-Link in DAP mode)
    │ (SWD: VCC, SWDIO, SWCLK, GND, NRST)
    ▼
[ Analog Mimic Board ] (J5)
```

SWD connector J5 (mimic01):

| Pin | Signal |
|---|---|
| 1 | VCC |
| 2 | SWDIO (PA13) |
| 3 | SWCLK (PA14) |
| 4 | GND |
| 5 | NRST |

1. Connect your CMSIS-DAP debugger to J5.
2. Power the module. The SWD signal levels follow the module supply (VCC). A debugger with 3.3 V logic may not drive a module running at 5 V reliably; flash at 3.3 V, or use a debugger that follows the target voltage (VTref).
3. Run the `pyocd flash` command. Upon successful completion, reset the MCU to start the 200 kHz sampling pipeline.
