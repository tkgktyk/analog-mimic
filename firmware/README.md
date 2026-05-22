# firmware/README.md

This directory contains the core firmware source code, peripheral drivers, and build configurations for the Analog Mimic module. This guide outlines the setup required for compiling the firmware from source and flashing the compiled binary onto the target microcontroller.

## 1. Development Environment Setup

Analog Mimic firmware is targeted for the Puya Semiconductor PY32F071 MCU (Arm Cortex-M0+, 72 MHz). To compile and flash the firmware, the following toolchains and utility tools are required.

### Required Software Components

1. **GCC ARM Embedded Toolchain (`arm-none-eabi-gcc`)**
* Used for compiling and linking the C/C++ source code into an ARM architecture binary.
* Ensure it is added to your system environment variable `PATH`.

2. **Build Automation Tool (`make` or `CMake`)**
* Standard GNU Make is utilized to execute the compilation pipeline via the `Makefile`.

3. **Python 3.x & pyOCD**
* pyOCD is an open-source Python library used for programming and debugging ARM Cortex microcontrollers via a CMSIS-DAP debugger.

---

## 2. Compilation and Build Pipeline

To compile the firmware, navigate to this directory and use the build configuration. The optimization flags are strictly configured (`-O2` or `-O3`) to meet the strict 240-clock-cycle budget for the 300 kHz primary ISR loop, and `.ramfunc` routes time-critical instructions into zero-wait-state SRAM.

### Build Commands

* **Compile Firmware:**
```bash
make all
```

*This generates the compiler outputs in the `build/` directory, resulting in `analog_mimic_MIMIC00.elf`, `.bin`, and `.hex` files.*
* **Clean Build Artifacts:**
```bash
make clean
```

---

## 3. Flashing & Debug Environment

Target programming is handled asynchronously via the SWD (Serial Wire Debug) pins using a standard CMSIS-DAP compliant debug probe.

### Target MCU Specification

* **Target Device:** Puya PY32F071xB
* **Memory Config:** 128 KB Flash / 16 KB SRAM

### Flash Programming via pyOCD

pyOCD natively supports the PY32 series by loading the corresponding device pack. Execute the following continuous command to flash the compiled production Intel HEX image onto the MCU:

```bash
pyocd flash -t py32f071xb analog_mimic_MIMIC00.hex

```

### Hardware Verification Setup

```
[ Host PC ]
    │ (USB)
    ▼
[ CMSIS-DAP Debugger ] (e.g., DAPLink, J-Link in DAP mode)
    │ (SWD Interface: SWCLK, SWDIO, GND, 3.3V)
    ▼
[ Analog Mimic Board ] (PY32F071 Target Pins)

```

1. Connect your CMSIS-DAP debugger to the SWD test points on the Analog Mimic PCB.
2. Power the module via the Pmod interface or the VDD pin (3.3V).
3. Run the `pyocd flash` command. Upon successful completion, reset the MCU to initiate the execution framework and launch the 300 kHz ADC sampling pipeline.
