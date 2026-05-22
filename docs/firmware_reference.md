
# Firmware Reference

This document provides a technical overview of the Analog Mimic firmware. It details the OtoHA execution framework, hardware resource allocations, internal DSP mechanics, and the I²C control interface, serving as a guide for users who wish to understand the core software pipeline.

## 1. Internal MCU Processing

The firmware pipeline executes sequentially within the hardware-locked Primary ISR to maintain absolute temporal determinism.

```
[Analog In]
   ↓
(1) ADC Read (12-bit Raw)
   ↓
(2) Mode-Specific DSP Core (32-bit state execution)
   ↓
(3) Common Post-processing 1: Branchless DAC Code Inversion & DC Offset
   ↓
(4) Common Post-processing 2: Fixed-point Saturation (Clamps strictly to 0 - 4095)
   ↓
(5) DAC Preload (Next cycle output)
   ↓
[Analog Out]

```

---

## 2. Hardware Resource Mapping

Analog Mimic is built around the Puya Semiconductor PY32F071, a low-cost 72 MHz Arm Cortex-M0+ MCU. The continuous-time signal chain defined in the architecture is mapped directly to the MCU's internal peripherals.

### Peripheral Assignment Table

| Hardware Resource | Internal Peripheral | Firmware Function / Mapping |
| --- | --- | --- |
| **System Core Clock** | HSI Oscillator | Configured to 72 MHz via internal PLL. |
| **Sampling Clock** | TIM3 (16-bit Advanced Timer) | Configured in up-counting mode to generate periodic Update Interrupts at exactly 300 kHz (Priority 0: Primary ISR). |
| **Input Buffer** | OPA1 | Configured as a Hi-Z voltage follower. |
| **Anti-Aliasing Filter (AAF)** | OPA3 | Configured as a 2nd-order Sallen-Key Low-Pass Filter ($Q = 0.5$, $f_c = 48.2$ kHz). |
| **Analog Input Interface** | ADC1 (12-bit, Successive Approx.) | Mapped to internal channels or pins depending on the variant. Set to high-speed single-conversion mode triggered automatically by the timer. |
| **Primary Analog Output** | DAC1 (12-bit R-2R Ladder) | Wired directly to the output smoothing chain (AOUT). Driven by an immediate software trigger (SWTRIGR = 1) at the very first instruction of the Primary ISR, latching the pre-calculated code. |
| **Anti-Imaging Filter (AIF)** | OPA2 | Configured as a 2nd-order Sallen-Key Low-Pass Filter ($Q = 0.5$, $f_c = 48.2$ kHz). |
| **Host Communication** | I2C1 (Hardware Block) | Mapped to Scl/Sda lines for host-level runtime reconfiguration (Priority 1: Secondary ISR). |

---

<!-- ## 3. OtoHA Execution Framework

Analog Mimic achieves hardware-like temporal determinism on a resource-constrained microcontroller by implementing the OtoHA (Optimization toward Hardware-deterministic Architecture) framework. To balance strict phase-accuracy with topological reconfigurability, the firmware bifurcates execution into two distinct states.

### Hardware and Software Mapping

```
+---------------------------------------------------------------------------------+
|                                 MCU EXECUTION                                   |
+---------------------------------------------------------------------------------+
|  [Deterministic State]                         [Non-Deterministic State]        |
|                                                                                 |
|  Primary ISR                                   Secondary ISR      Main Loop     |
|  (Priority 0)                                  (Priority 1)       (Background)  |
|       │                                             │                  │        |
|       ▼                                             ▼                  ▼        |
|  Tied directly to:                             Tied to:           Tied to:      |
|  300 kHz Hardware Timer                        Asynchronous       Parameter     |
|  & Periodic ADC Sampling                       I2C Transactions   Decoding &    |
|                                                                   Q15 Scaling   |
+---------------------------------------------------------------------------------+

```

### Deterministic State (Mapped to Primary ISR & 300 kHz ADC Sampling)

* **Hardware Binding:** This state is implemented inside the Primary ISR, which is tied directly to a hardware timer interrupt set to a strict 300 kHz sampling rate (3.3 µs sample period). This timer commands the ADC to initiate conversions at absolute periodic intervals.
* **Execution Pipeline:** Executes the time-critical mixed-signal chain. Upon the 300 kHz hardware trigger, the CPU bypasses standard interrupt latencies to issue an immediate software trigger to the DAC (presenting the pre-calculated analog code from the previous sample). It then reads the newly converted 12-bit raw value from the ADC, routes it through the active 32-bit promoted DSP emulation core, and performs common post-processing (branchless inversion, offset, and clamping) to preload the DAC for the next cycle.
* **Zero-Jitter Optimizations:** To guarantee sub-clock execution determinism (<13 ns jitter) across the 300 kHz ADC boundary, the NVIC vector table and the entire Primary ISR instruction routine are moved to zero-wait-state SRAM via the `.ramfunc` attribute. Active emulation explicitly suspends asynchronous system background tasks (such as the standard SysTick handler) to prevent nested preemption.

### Non-Deterministic State (Mapped to I²C Interrupts & Main Loop)

* **Hardware Binding:** This state handles external host communication and is bifurcated into the I²C peripheral interrupt (Secondary ISR) and the background Main Loop.
* **Execution Pipeline:** Manages topological and parameter updates. When a host microcontroller (e.g., an Arduino) issues a configuration command, the hardware I²C interrupt (Secondary ISR) triggers at a lower priority (Priority 1) to capture incoming payload bytes into a temporary data buffer without interrupting the active Primary ISR. Once the transfer completes, the background Main Loop takes over to decode the commands, translate human-readable physical parameters (such as cutoff frequencies in Hz or decay times in microseconds) into digital raw values or Q15 fixed-point coefficients, and safely hot-swaps the DSP routing mode.

### Cycle Budget Allocation

Operating a 72 MHz Cortex-M0+ core at a 300 kHz sampling rate compresses the total single-sample computational window into a strict budget of 240 clock cycles per sample.

```
5 µs Sample Period @ 72 MHz Core Clock
|←───────────────────────── 240 Clock Cycles ─────────────────────────→|
┌───────────────────────────────────────────────┬──────────────────────┐
│ Core DSP Execution                            │ Required Slack       │
│ (e.g., Fixed-Point Emulation Routine)         │ (60 - 70 Cycles)     │
└───────────────────────────────────────────────┴──────────────────────┘
                                                ▲
                                                └── Mapped to:
                                                    - I2C ISR Overhead
                                                    - Main Loop Decoding

```

* **DSP Execution:** Highly optimized fixed-point routines execute inside the Primary ISR, consuming a predictable chunk of the sample budget.
* **Slack Margin:** The framework rigorously maintains a required slack margin of 60 to 70 clock cycles outside of the core DSP execution. This deterministic cycle reserve is dedicated entirely to non-DSP operations—specifically handling the I²C Secondary ISR communication overhead and allowing the background Main Loop to calculate new filter coefficients before the next WFI (Wait-For-Interrupt) synchronization boundary.

--- -->

## 4. DSP Core & Fixed-Point Arithmetic

To maximize efficiency within the 240-cycle budget, the firmware completely avoids runtime floating-point arithmetic. All mathematical emulation blocks are implemented using a 32-bit promoted Q15 fixed-point representation.

### Fixed-Point Format & 32-bit Promotion

In the Q15 format, a signed integer represents a fractional value in the range $[-1.0, 1.0 - 2^{-15}]$:

$$\text{Value}_{\text{Float}} = \frac{\text{Internal integer}}{32768}$$

To eliminate memory alignment padding gaps and avoid redundant CPU cast instructions on the 32-bit Cortex-M0+ architecture, all internal DSP states and arithmetic operations are natively promoted to `int32_t` (`FRACTIONAL_BITS_Q15 = 15`).

* **Addition / Subtraction:** Executed natively using 32-bit registers.
* **Multiplication:** Performed using native 32-bit arithmetic. The 32-bit intermediate product is immediately shifted right by 15 bits (`>> 15`) to maintain the Q15 fractional scale, without dropping back to 16 bits until the final DAC bounds clamping.

### Parameter Processing & Scaling

When a host passes a physical parameter (such as a cutoff frequency in Hz or an integration time constant in microseconds) over I²C, the firmware does not use these values directly inside the Primary ISR. Instead, the Secondary ISR intercepts the parameters and converts them into normalized digital coefficients (such as $b_0, b_1, a_1$ coefficients or pre-calculated decay multipliers) using a background fixed-point conversion library.

* **Voltage Inputs (mV to Raw 12-bit ADC/DAC Codes):**
* Unsigned:

$$\text{Raw} = \frac{\text{mV} \times 4095}{V_{DD\_mV}}$$


* Signed (Centered around $V_{DD}/2$ mid-rail):

$$\text{Raw}_{\text{Signed}} = \frac{\text{mV} \times 2048}{V_{DD\_mV}/2}$$




* **Frequency Inputs ($f_c$ in Hz to Biquad Coeffs):**
Pre-warped digital angular frequencies $\omega_0 = 2\pi f_c / f_s$ are mapped to Q15 filter coefficients using optimized Taylor-series polynomial approximations for sine and cosine.

### Parameter Specification & Payload Types Table

When the host configures a DSP mode, it sends parameters over I²C using specific data types. The following table defines these payload types, classifying how human-readable physical units are translated into the raw bytes utilized by the DSP core.

| Payload Type | Logical Parameter | User/Host Unit | Hardware / Internal Mapping |
| --- | --- | --- | --- |
| **S16** *(Signed 16-bit)* | Q15 Coefficient (Freq $f_c$, Time Const $\tau$, Gain) | Hz, $\mu$s, or Float | Converted by the host into Q15 fractional format $[-1.0, 1.0)$. Used internally for digital filters ($b_n, a_n$), integration/decay factors, and fractional scaling. |
| **S16 / U16** *(16-bit Integer)* | Raw ADC Code / Offset (Threshold, $V_{\text{ref}}$, DC Offset) | mV | Transformed by the host from physical voltage to a 12-bit raw integer $[0 - 4095]$. U16 is typically used for absolute thresholds, while S16 is used for offsets and clipping limits. |
| **S8** *(Signed 8-bit)* | Bit-Shift Multiplier (Coarse Gain) | Integer | Used in conjunction with Q15 fractional multipliers. Performs fast coarse scaling via bit-shifting (e.g., $\ll 2$ for $4\times$ gain, or $\gg 1$ for attenuation). |
| **U16** *(Unsigned 16-bit)* | Sample Count (Delay time, S&H period) | Samples (Integer) | Represents an exact number of 300 kHz clock cycles. Used directly for delay buffer sizing and downsampling counters. |
| **U8** *(Unsigned 8-bit)* | Flag / Enumeration (Polarity, LUT Type) | Boolean / Enum | Simple 1-byte selection codes. Maps to conditional branching flags (e.g., peak vs. trough) or Look-Up Table target pointers. |

---

## 5. I²C Interface & Mode Specifications

The internal topology of the Analog Mimic is fully reconfigurable at runtime via an 8-bit memory-mapped I²C register interface.

### Register Map Overview

The device utilizes a fixed memory map bridging host interactions and the internal DSP state machine. Registers 0x00 through 0x1F reside in physical SRAM (`mimic_device.registers`), whereas telemetry addresses (0x20+) are virtual and dynamically latched upon host read requests to guarantee thread safety.

| Address | Register Name | Access | Width | Description |
| --- | --- | --- | --- | --- |
| **0x00** | MIMIC_REG_GLOBAL_FLAGS | R/W | 8-bit | System overrides. Bit 7 (0x80): Output Invert, Bit 6 (0x40): Output Open (Hi-Z), Bit 5 (0x20): Servo Enable. |
| **0x01** | MIMIC_REG_OUTPUT_OFFSET_H | R/W | 8-bit | MSB of global DC offset applied universally post-DSP. |
| **0x02** | MIMIC_REG_OUTPUT_OFFSET_L | R/W | 8-bit | LSB of global DC offset. |
| **0x10** | MIMIC_REG_MODE_SELECT | R/W | 8-bit | Selects the active DSP routing topology (Mode ID). |
| **0x11+** | MIMIC_REG_PAYLOAD_START | R/W | Block | Mode-specific parameter buffer. Interpreted based on MODE_SELECT. |
| **0x20** | MIMIC_REG_STATUS | R/C | 8-bit | System Status Flags (Bit 0: Ready, Bit 1: Saturation, Bit 2: I2C Error, Bit 3: ADC Over Error). Cleared on read. |
| **0x21** | MIMIC_REG_ADC_VAL_H | R | 8-bit | MSB of the live 12-bit ADC tracking conversion. |
| **0x22** | MIMIC_REG_ADC_VAL_L | R | 8-bit | LSB of the live 12-bit ADC conversion. |
| **0x23** | MIMIC_REG_CPU_CYCLES_H | R/C | 8-bit | MSB of peak execution cycles per sample. Cleared on read. |
| **0x24** | MIMIC_REG_CPU_CYCLES_L | R/C | 8-bit | LSB of peak execution cycles per sample. |

### Operational Mode Specifications

The host triggers a pipeline reconfiguration by writing the target Mode ID to `MIMIC_REG_MODE_SELECT` (0x10), followed by mode-specific bytes starting at `MIMIC_REG_PAYLOAD_START` (0x11).

#### 0x00: DSP Bypass (`MIMIC_MODE_ID_BYPASS_DSP`)

* **Function:** Acts as a strictly deterministic voltage follower.
* **Implementation:** The ADC input is routed directly to the DAC. Global pipeline effects (Inversion, Offset) remain active.
* **Payload Parameters:** None.

#### 0x01: Programmable Gain Amplifier (`MIMIC_MODE_ID_PGA`)

* **Function:** Scales the input signal linearly with respect to an arbitrary reference center.
* **Implementation:** Combines a Q15 fractional multiplier for fine-tuning and a bit-shift operation for gross hardware-like amplification.
* **Payload Parameters:** * 0x11-0x12 (S16): `gain_fract_q15` - Fractional gain multiplier.
* 0x13 (S8): `gain_shift` - Coarse bit-shift multiplier.
* 0x14-0x15 (S16): `offset_raw` - DC offset reference center (Raw ADC code). Internally mapped to Q15.



#### 0x02: 1st-Order Low-Pass Filter (`MIMIC_MODE_ID_FILTER_1ST_LPF`)

* **Function:** Emulates a continuous-time RC low-pass filter.
* **Implementation:** Standard infinite impulse response (IIR) single-pole accumulator.
* **Payload Parameters:**
* 0x11-0x12 (S16): `alpha_q15` - Decay/Integration coefficient derived from target $f_c$.



#### 0x03: 1st-Order High-Pass Filter (`MIMIC_MODE_ID_FILTER_1ST_HPF`)

* **Function:** Emulates an RC high-pass network (useful for automatic DC-blocking/servo tracking).
* **Implementation:** Single-pole accumulator with the state subtracted from the raw input.
* **Payload Parameters:**
* 0x11-0x12 (S16): `alpha_q15` - Cutoff frequency coefficient.



#### 0x04: 2nd-Order Biquad Filter (`MIMIC_MODE_ID_FILTER_BIQUAD`)

* **Function:** Universal 2nd-order digital filter configurable as LPF, HPF, BPF, or Notch.
* **Implementation:** Direct Form II Transposed (DF2T). Highly unrolled loop loading coefficients aggressively into CPU registers. Dominates the processing budget (~212 cycles).
* **Payload Parameters:**
* 0x11-0x12 (S16): `b0` - Feedforward coefficient 0.
* 0x13-0x14 (S16): `b1` - Feedforward coefficient 1.
* 0x15-0x16 (S16): `b2` - Feedforward coefficient 2.
* 0x17-0x18 (S16): `a1` - Feedback coefficient 1.
* 0x19-0x1A (S16): `a2` - Feedback coefficient 2.



#### 0x05: Window Comparator (`MIMIC_MODE_ID_COMPARATOR_WIN`)

* **Function:** Triggers output HIGH when the signal escapes the defined upper/lower boundaries.
* **Implementation:** Pure conditional branching logic outputting absolute 12-bit limits.
* **Payload Parameters:**
* 0x11-0x12 (U16): `upper` - Raw ADC threshold code (Upper bound).
* 0x13-0x14 (U16): `lower` - Raw ADC threshold code (Lower bound).



#### 0x06: Schmitt Trigger (`MIMIC_MODE_ID_COMPARATOR_SCHMITT`)

* **Function:** A comparator with hysteresis memory, preventing noisy transitions.
* **Implementation:** Conditional branching retaining state memory (`wave_last_raw`).
* **Payload Parameters:**
* 0x11-0x12 (U16): `upper` - Trigger HIGH threshold.
* 0x13-0x14 (U16): `lower` - Trigger LOW threshold.



#### 0x07: Envelope Follower (`MIMIC_MODE_ID_ENVELOPE_FOLLOWER`)

* **Function:** Tracks amplitude peaks (or troughs) with a customizable decay release time. Acts as a Peak Hold if decay is zero.
* **Implementation:** Multiplies the historical state by a Q15 decay factor when the input falls below the tracked envelope.
* **Payload Parameters:**
* 0x11-0x12 (S16): `decay_q15` - Q15 decay factor (derived from $\tau$ us).
* 0x13 (U8): `polarity` - Positive peak (0) or Negative trough (1) tracking.



#### 0x08: Signal Clipper (`MIMIC_MODE_ID_CLIPPER`)

* **Function:** Clamps the output signal strictly within absolute physical voltage boundaries.
* **Implementation:** Standard boundary threshold branching limits.
* **Payload Parameters:**
* 0x11-0x12 (S16): `upper` - Upper clamp limit (Raw).
* 0x13-0x14 (S16): `lower` - Lower clamp limit (Raw).



#### 0x09: Non-linear LUT Waveshaper (`MIMIC_MODE_ID_NONLINEAR_LUT`)

* **Function:** Emulates analog logarithmic and anti-logarithmic amplification curves.
* **Implementation:** Instead of heavy runtime Taylor series execution, maps a pre-computed 65-point Look-Up Table (LUT) across the ADC range, utilizing single-cycle linear interpolation for sub-index accuracy.
* **Payload Parameters:**
* 0x11 (U8): `lut_type` - 0 for Logarithmic, 1 for Anti-Logarithmic.



#### 0x0A: Signal Derivative (`MIMIC_MODE_ID_MATH_DERIVATIVE`)

* **Function:** Mathematical differentiator (extracts the rate of change).
* **Implementation:** Calculates $\Delta$ between the current and previous sample, scaled by a fractional Q15 multiplier and bit-shift.
* **Payload Parameters:**
* 0x11-0x12 (S16): `scale_fract_q15` - Base scaling multiplier.
* 0x13 (S8): `scale_shift` - Coarse gain shift multiplier.



#### 0x0B: Leaky Integral (`MIMIC_MODE_ID_MATH_INTEGRAL`)

* **Function:** Accumulates the input signal over time.
* **Implementation:** Standard accumulator that slightly attenuates (leaks) the historical state using a fractional multiplier to prevent unbounded signal saturation.
* **Payload Parameters:**
* 0x11-0x12 (S16): `scale_fract_q15` - Leaky attenuation factor for historical state.
* 0x13 (S8): `scale_shift` - Dynamic gain scale for new incoming samples.



#### 0x0C: Full-Wave Rectifier (`MIMIC_MODE_ID_RECTIFIER_FULL`)

* **Function:** Absolute value mathematical operation (folds negative half-cycles up).
* **Implementation:** Branches around a defined center reference voltage (`vref_raw`).
* **Payload Parameters:**
* 0x11-0x12 (S16): `vref_raw` - Symmetrical center voltage reference code.



#### 0x0D: Slew Rate Limiter (`MIMIC_MODE_ID_SLEW_RATE_LIMITER`)

* **Function:** Limits how fast the voltage can physically change per sample ($\Delta V / \Delta t$).
* **Implementation:** Calculates the delta and clamps it mathematically without heavy pipeline-flushing jumps (branchless clamp logic).
* **Payload Parameters:**
* 0x11-0x12 (S16): `scale_fract_q15` - Maximum allowed delta per 300 kHz sample.



#### 0x0E: Sample & Hold (`MIMIC_MODE_ID_SAMPLE_AND_HOLD`)

* **Function:** Freezes the analog signal at a specified interval (downsampling).
* **Implementation:** Utilizes an internal sample counter to hold `last_q15` constant until the tracking threshold is breached.
* **Payload Parameters:**
* 0x11-0x12 (U16): `period_samples` - Total duration of the T&H cycle.
* 0x13-0x14 (U16): `track_samples` - Duration spent tracking before holding.



#### 0x0F: Digital Delay (`MIMIC_MODE_ID_DELAY`)

* **Function:** Pure time-delay buffer without filtering.
* **Implementation:** Allocates a contiguous ring buffer (`delay_buffer[4096]`) masking the write pointer mathematically for safe wraparound.
* **Payload Parameters:**
* 0x11-0x12 (U16): `delay_samples` - Number of samples to hold in memory.
