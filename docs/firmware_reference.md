# Firmware Reference

This document describes the Analog Mimic firmware: the sampling pipeline, the mapping to the MCU peripherals, the fixed-point arithmetic, and the I²C register interface. `firmware/core/Inc/mimic_registers.h` is the authoritative definition of the interface; this document follows it.

## 1. Sampling Pipeline

Every sample period (200 kHz, 5 µs) the firmware runs the following chain in the primary ISR.

```
[Analog In]
   ↓
(1) ADC conversion, started by the sampling timer (12-bit raw code)
   ↓
(2) Decimation (optional): average 2^N samples, run (3)-(5) once per block
   ↓
(3) Mode-specific DSP core (32-bit integer arithmetic)
   ↓
(4) Output stage: calibration gain, inversion, DC offset (user + calibration), rounding
   ↓
(5) Saturation to 0 - 4095
   ↓
(6) Output preload: written at a fixed point of the next sample period
    (DAC code on the analog route, GPIO level on the digital route)
   ↓
[Analog Out]
```

With decimation, the output between two processed samples is linearly interpolated.

The value computed in one sample period is presented at a fixed point of the next one, so the output timing does not depend on how long the DSP core takes.

---

## 2. Hardware Resource Mapping

Analog Mimic is built around the Puya Semiconductor PY32F071, a 72 MHz Arm Cortex-M0+ MCU. The analog signal chain is built from the MCU's internal peripherals.

| Function | Peripheral | Configuration |
| --- | --- | --- |
| **System clock** | HSI + PLL | 72 MHz. |
| **Sampling clock** | TIM2 | 200 kHz (period 360 clocks). CH2 triggers the ADC. |
| **Input buffer** | OPA1 | Voltage follower. |
| **Anti-aliasing filter (AAF)** | OPA3 | 2nd-order Sallen-Key low-pass filter ($Q = 0.5$, $f_c = 48.2$ kHz). |
| **ADC** | ADC1 (12-bit SAR) | Channel 9 (AAF output), triggered by TIM2 CH2. Reference: VCCA (ratiometric to VDD). |
| **DAC** | DAC1 (12-bit, buffered) | Analog output route. |
| **Anti-imaging filter (AIF)** | OPA2 | 2nd-order Sallen-Key low-pass filter ($Q = 0.5$, $f_c = 48.2$ kHz) driving AOUT. |
| **Digital output route** | GPIO (AOUT pin) | Comparator output. OPA2 and the DAC are stopped while this route is active. |
| **Host interface** | I2C1 | 100 kHz, 7-bit address 0x40. Lower-priority interrupt. |

The whole analog chain from AIN to AOUT (AAF, sampling and AIF) has a −3 dB bandwidth of 20 kHz.

### Output engines (build option)

The agent that writes the output is selected at build time (`ENGINE`).

* **DMA (default):** the ADC end-of-conversion requests a DMA transfer that writes the preloaded output word to the DAC (or to the GPIO set/reset register). The DMA transfer-complete interrupt then runs the DSP for the new sample and prepares the next output word.
* **CPU:** the ADC interrupt writes the preloaded output (DAC software trigger or GPIO write) as its first action, then runs the DSP.

In both engines the primary ISR and the vector table run from SRAM (`.ramfunc`), and SysTick is suspended while the engine runs.

### Cycle budget

At 72 MHz and 200 kHz, one sample period is **360 CPU cycles**. The DSP core and the output stage must leave a margin (about 60 - 70 cycles) for the I²C interrupt and the main loop. The peak cycle count of the primary ISR can be read from the `CPU_CYCLES` register.

---

## 3. Fixed-Point Arithmetic

The DSP core uses no floating point. All states and arithmetic are 32-bit integers (`int32_t`), and every product fits in 32 bits, because the Cortex-M0+ has no 32 × 32 → 64-bit multiply instruction. Coefficients are calculated by the host (Arduino library) and sent over I²C; the firmware only parses them.

### Number formats

| Name | Meaning |
| --- | --- |
| `raw` | ADC / DAC code. 0 = 0 V, 4096 = VDD (integer, 0 - 4095). 1 LSB = VDD / 4096. |
| `raw_fx3` | `raw` with 3 fractional bits (1 LSB = 8), signed 16-bit, -4096 to +4095.875 LSB. **Every voltage at the interface uses this format.** |
| `raw_vref_fx3` | `raw_fx3` relative to Vref (internal). |
| `q0_15` | Signed coefficient, 15 fractional bits (-1.0 to < 1.0). |
| `uq1_15` | Unsigned gain, 15 fractional bits (0 to < 2.0, 1.0 = 32768). |
| `mant` / `exp` | value = mant (`q0_15`) × 2^exp (exp: int8 shift). |

All multi-byte values are big-endian.

Voltages are ratiometric: the ADC and the DAC both use VDD as the reference. The library converts millivolts to `raw_fx3` as `mV × 4096 × 8 / VDD_mV` (rounded), using the supply voltage given to the constructor.

### Common reference (Vref)

Vref is an input of the module (default VDD / 2, limited to 0 V - VDD). Modes with a signal center work as

$$y = V_{ref} + H(x - V_{ref})$$

Changing Vref does not reset any state; for example, a slow HPF settles to the new Vref with its own time constant.

* **Uses Vref:** 1st-order filter, Biquad, PGA, envelope follower, derivative / integral, rectifier, and the bipolar side of LOG.
* **Absolute (0 V based):** comparator and clipper thresholds, the LOG input and the ANTILOG output.
* **No signal center:** bypass, slew rate limiter, sample & hold, delay.

### Output stage

The output stage combines the calibration gain, the inversion, the DC offsets and the rounding in one multiply-add:

$$\text{out} = \left\lfloor \frac{y \cdot G + O}{2^{15}} \right\rfloor$$

where *G* is the calibration gain (`uq1_15`, negative when the output is inverted) and *O* holds the user offset and the calibration offset (`raw_fx3`, scaled to 15 fractional bits) plus 0.5 LSB for rounding. The result is saturated to 0 - 4095.

### Calibration

The gain and offset calibration values are stored in flash (NVM) and loaded at start-up. They correct the combined error of the input and output signal chains (measured from AIN to AOUT in bypass mode); the ADC and the DAC are not calibrated separately. A module that has not been calibrated uses gain 1.0 and offset 0. See the `NVM_*` registers and the commands below.

---

## 4. I²C Interface

The module is an I²C target at address 0x40 (7-bit; the library adds a jumper offset of 0 - 15). Registers are 8-bit wide and addressed with auto-increment. Registers 0x00 - 0x0F are read-only; 0x10 - 0x3F are kept in RAM and can be read back.

### Register map

| Address | Name | Access | Format | Description |
| --- | --- | --- | --- | --- |
| 0x00 | `SYSTEM_ID` | R | uint8 | WHO_AM_I, 0x4D (`'M'`). |
| 0x01 | `SYSTEM_HW_VERSION` | R | uint8 | Hardware version. |
| 0x02 - 0x05 | `SYSTEM_BUILD_DATE` | R | uint32 | Firmware build date, hex `0xYYYYMMDD` (e.g. 0x20261025). |
| 0x06 - 0x09 | `SYSTEM_UID` | R | uint32 | Unit ID: 32-bit FNV-1a hash of the MCU unique ID. |
| 0x0A | `SYSTEM_STATUS` | R/C | flags | Bit 0: ready, bit 1: signal saturation, bit 2: I²C error, bit 3: ADC overrun. Error bits are cleared on read. |
| 0x0B - 0x0C | `SYSTEM_ADC_VAL` | R | uint16 | Latest ADC code (latched on the high-byte read). |
| 0x0D - 0x0E | `SYSTEM_CPU_CYCLES` | R/C | uint16 | Peak cycles of the primary ISR since the last read. |
| 0x10 | `GLOBAL_FLAGS` | R/W | flags | Bit 7: invert output, bit 6: output open (Hi-Z), bit 4: servo enable (1x series only, reserved). |
| 0x11 | `GLOBAL_DECIMATION_N` | R/W | uint8 | Processing rate = 200 kHz / 2^N, N = 0 - 16 (larger values are clamped). |
| 0x12 - 0x13 | `GLOBAL_OFFSET` | R/W | int16 `raw_fx3` | User output offset. |
| 0x14 - 0x15 | `GLOBAL_VREF` | R/W | int16 `raw_fx3` | Common reference voltage (default VDD / 2). |
| 0x20 | `MODE_ID` | R/W | uint8 | Active mode (see below). |
| 0x21 - 0x2F | `MODE_PAYLOAD` | R/W | block | Mode parameters. |
| 0x30 - 0x31 | `NVM_GAIN` | R/W | uint16 `uq1_15` | Output gain calibration. |
| 0x32 - 0x33 | `NVM_OFFSET` | R/W | int16 `raw_fx3` | Output offset calibration. |
| 0x40 | `CMD_PORT` | W | uint8 | Command: 0xA5 = save the NVM registers to flash, 0x5A = reload them from flash. |

### Applying a configuration

The host writes the global registers, the mode ID and the payload; the firmware applies them as one update. During the update the output keeps its previous value. The output is disconnected (open) briefly only when the output route changes between the analog route (DAC) and the digital route (GPIO, comparator mode). Changing the mode resets the state of the mode; changing only the parameters of the current mode keeps its state.

---

## 5. Modes

Payload offsets are relative to `MODE_PAYLOAD` (0x21). Modes that group several types have the type at byte [0].

| ID | Mode | Payload |
| --- | --- | --- |
| 0x00 | `BYPASS_DSP` | none |
| 0x01 | `PGA` | [0-1] gain mant `q0_15`, [2] gain exp int8 (≥ 0) |
| 0x02 | `FILTER_1ST` | [0] type (0: LPF, 1: HPF), [1-2] alpha `q0_15` |
| 0x03 | `FILTER_BIQUAD` | [0] type, [1-10] coefficients (see below) |
| 0x04 | `COMPARATOR` | [0] type (0: window, 1: Schmitt), [1-2] upper `raw_fx3`, [3-4] lower `raw_fx3` |
| 0x05 | `CLIPPER` | [0-1] upper `raw_fx3`, [2-3] lower `raw_fx3` |
| 0x06 | `RECTIFIER` | [0] type (0: full, 1: half), [1] polarity (0: positive, 1: negative) |
| 0x07 | `ENVELOPE_FOLLOWER` | [0-1] decay `q0_15`, [2] polarity |
| 0x08 | `LOG` | [0] type (0: log, 1: antilog), [1-2] slope `raw_fx3` per decade (signed), [3-4] unit `raw_fx3` (> 0) |
| 0x09 | `MATH` | [0] type (0: derivative, 1: integral), [1-2] mant `q0_15`, [3] exp int8 |
| 0x0A | `SLEW_RATE_LIMITER` | [0-1] maximum change per sample `raw_fx3` (≥ 0) |
| 0x0B | `SAMPLE_AND_HOLD` | [0-1] period, [2-3] track time (samples, uint16) |
| 0x0C | `DELAY` | [0-1] delay (samples, uint16, up to 4095) |

### 0x00 Bypass
Voltage follower. The output stage (calibration, inversion, offset) still applies.

### 0x01 PGA
$y = V_{ref} + G (x - V_{ref})$ with $G = \text{mant} \cdot 2^{\text{exp}}$.

### 0x02 1st-order filter
RC low-pass or high-pass filter around Vref: a single-pole accumulator with $\alpha$ derived from $f_c$; the HPF output is the input minus the LPF state.

### 0x03 Biquad (2nd-order filter)
LPF, HPF, BPF (0 dB peak gain), notch, or unity (input passed through with the same CPU load, for profiling).

Implemented as a **TPT (topology-preserving transform) state-variable filter**: the integrators of an analog state-variable filter are discretized with trapezoidal integration, which gives the same response as a bilinear-transform biquad (Audio EQ Cookbook). With $g = \tan(\pi f_c / f_s)$ and $k = 1/Q$:

```
v  = x - (k + g) s1 - s2,   hp = v / (1 + g k + g^2)
bp = g hp + s1,   s1 = bp + g hp
lp = g bp + s2,   s2 = lp + g bp
LPF = lp,  HPF = hp,  BPF = k bp,  NOTCH = x - k bp
```

The signals are `raw_vref_fx3` and the two states have 15 fractional bits, so low cutoffs keep working without decimation. The coefficients are sent as 12-bit mantissas with shifts, so their resolution does not depend on $f_c$:

| Bytes | Content |
| --- | --- |
| [0] | type: 0 LPF, 1 HPF, 2 BPF, 3 notch, 4 unity |
| [1-2], [3] | $g \cdot 2^{12}$ = mant >> shift (mant 2048 - 4095) |
| [4-5] | $k + g$, 10 fractional bits |
| [6-7], [8] | $e = 1 - 1/(1 + g k + g^2)$ = mant >> shift |
| [9-10] | $k$, 11 fractional bits |

Limits applied by the library: $f_s / 100000 \le f_c \le 0.24 f_s$ (2 Hz - 48 kHz at 200 kHz) and $0.1 \le Q \le 10$.

### 0x04 Comparator
Digital output route (GPIO). Window: HIGH when the input is above the upper or below the lower threshold. Schmitt: HIGH above the upper threshold, LOW below the lower threshold, otherwise unchanged.

### 0x05 Clipper
Limits the input to the lower - upper range (absolute voltages).

### 0x06 Rectifier
Folds the signal at Vref. Full-wave positive: $V_{ref} + |x - V_{ref}|$; half-wave positive: $\max(x, V_{ref})$; negative polarity mirrors both.

### 0x07 Envelope follower
Follows the peak of the input above Vref (positive) or below Vref (negative) and decays by the decay factor per sample. A decay factor of 1.0 holds the peak. The output is Vref plus the tracked magnitude for both polarities.

### 0x08 LOG
Logarithmic amplifier: $y = V_{ref} + \text{slope} \cdot \log_{10}(x / \text{unit})$. Antilogarithmic: $y = \text{unit} \cdot 10^{(x - V_{ref}) / \text{slope}}$. Implemented with per-mode tables (64 segments per octave) built when the mode is configured, and linear interpolation.

### 0x09 Math
Derivative: $y = V_{ref} + s \cdot \Delta x$ with $s = \text{mant} \cdot 2^{\text{exp}}$. Leaky integrator: every sample, the state is multiplied by the leak factor (mant) and the input relative to Vref, scaled by $2^{\text{exp}}$ (exp ≤ 1; negative values attenuate), is added. The output is Vref plus the state.

### 0x0A Slew rate limiter
Limits the change of the output per sample.

### 0x0B Sample & hold
Tracks the input for the first *track* samples of every *period* samples and holds the last value for the rest. track = 1 gives a sample & hold; larger values give a track & hold.

### 0x0C Delay
Delays the input by the given number of samples (ring buffer of 4096 samples).
