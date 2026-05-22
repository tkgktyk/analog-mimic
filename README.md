# Analog Mimic (analog-mimic)

<!-- ![Analog Mimic Board](docs/images/analog_mimic_board.jpg) -->

## Overview
Analog Mimic is a drop-in, programmable analog front-end (AFE) module powered by a robust "MCU + DSP" architecture. It is designed to bring software-defined flexibility to physical analog boundaries.

**Key Features:**
* **Versatile Emulation:** Physically mimics diverse analog circuits (e.g., filters, amplifiers, comparators).
* **Instant Reconfiguration:** Fully reconfigurable on the fly with a single I²C command.
* **Drop-in Ready:** Designed to be easily integrated into your existing hardware prototyping workflow.


<!-- ## Underlying Technology
**Analog Mimic is powered by OtoHA.**

OtoHA (Optimization toward Hardware-deterministic Architecture) is the foundational framework established in our IEEE ESL paper: *"Sub-Clock Hardware-Deterministic Architecture for Phase-Accurate Analog Emulation on Resource-Constrained MCUs"*. 

By isolating the critical execution path and eliminating architectural timing penalties, OtoHA ensures strict time-invariance and hardware-like precision, allowing standard microcontrollers to operate as strictly deterministic mixed-signal blocks. -->

## Hardware Concept
* **Form Factor:** Pmod Type 6 (I²C) compatible layout with a compact, breadboard-friendly footprint (35.56 mm × 20 mm).
* **On-Chip Integration:** Seamless integration of internal operational amplifier-based input/output buffers, anti-aliasing filters (AAF), and anti-imaging filters (AIF) mapped directly alongside the high-speed processing core.

## Software Ecosystem
To lower the barrier to entry for real-time digital signal processing, we provide a dedicated **C++ library for Arduino**. 
* **Zero-Hassle Control:** Complex Q15 fixed-point arithmetic and low-level I²C register maps are completely abstracted away.
* **Intuitive High-Level API:** Users can dynamically switch emulation modes and fine-tune physical parameters—such as cutoff frequencies, amplifier gains, or envelope release times—by simply passing variables into straightforward function calls.

*For comprehensive details on the inner workings, execution framework, and API configurations, please refer to [firmware_reference.md](docs/firmware_reference.md) and the Doxygen-annotated [Mimic.h](software/Mimic/src/Mimic.h).*

## Product Lineup
* **mimic0x (Standard Type):** The base model optimized for standard analog emulation and generalized mixed-signal prototyping.
* **mimic1x (AFE Extended Type):** An enhanced version featuring expanded hardware-level analog front-end scaling for specialized sensor interfacing.
