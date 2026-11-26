# Firmware Reference

This document describes the Analog Mimic firmware: the sampling pipeline, the mapping to the MCU peripherals, the fixed-point arithmetic, and the I²C register interface. `firmware/core/Inc/mimic_registers.h` is the authoritative definition of the interface; this document follows it.

## 1. Sampling Pipeline

Every sample period (200 kHz, 5 µs) the firmware runs the following chain in the primary ISR.

```
[Analog In]
   ↓
(1) ADC conversion, started by the sampling timer (12-bit raw code)
   ↓
(2) Conversion to raw_fx3 (x 8); with decimation, the rounded average of 2^N
    samples, and (3)-(5) run once per block
   ↓
(3) Mode-specific DSP core (32-bit integer arithmetic, raw_fx3 in and out)
   ↓
(4) Output stage: calibration gain, inversion, DC offset (user + calibration),
    and the only rounding to the integer DAC code
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

At 72 MHz and 200 kHz, one sample period is **360 CPU cycles**. The DSP core and the output stage must leave a margin (about 60 - 70 cycles) for the I²C interrupt and the main loop. The peak cycle count of the primary ISR can be read from the `CPU_CYCLES` register; it covers the body of the ISR. The whole ISR takes about 75 cycles more: about 50 for saving and restoring the registers and about 25 for the exception entry and exit.

If the ISR of a sample has not finished when the next sample starts, the `DEADLINE_MISS` status bit is set (checked at the end of the ISR body; only the register restore and the return, about 12 cycles, come after it). DMA engine: the next ADC conversion has already moved the slot to the output. CPU engine: the next conversion has finished, so its ISR and output trigger start late. With decimation the output of every sample is the interpolated value, written before the DSP core runs, so a late ISR on the DSP sample may still output on time; the bit is set anyway, because the whole ISR must fit in one sample period.

---

## 3. Fixed-Point Arithmetic

The DSP core uses no floating point. States and arithmetic are 32-bit integers (`int32_t`), and every product fits in 32 bits, because the Cortex-M0+ has no 32 × 32 → 64-bit multiply instruction. The only 64-bit value is the mean square of the true RMS mode, which is updated with additions of 32-bit products. Coefficients are calculated by the host (Arduino library) and sent over I²C; the firmware only parses them.

### Number formats

| Name | Meaning |
| --- | --- |
| `raw` | ADC / DAC code. 0 = 0 V, 4096 = VDD (integer, 0 - 4095). 1 LSB = VDD / 4096. |
| `raw_fx3` | `raw` with 3 fractional bits (1 LSB = 8), signed 16-bit, -4096 to +4095.875 LSB. **Every voltage at the interface and every signal inside the DSP uses this format**; only the ADC read and the DAC write use integer codes. |
| `raw_vref_fx3` | `raw_fx3` relative to Vref (internal). |
| `q0_15` | Signed coefficient, 15 fractional bits (-1.0 to < 1.0). |
| `uq1_15` | Unsigned gain, 15 fractional bits (0 to < 2.0, 1.0 = 32768). |
| `mant` / `exp` | value = mant (`q0_15`) × 2^exp (exp: int8 shift). |
| `mant12` / shift | positive coefficient $c < 1$ = mant / 2^shift with a 12-bit mantissa (2048 - 4095) and shift ≥ 12; mant = 0 means $c = 0$. |

All multi-byte values are big-endian.

Voltages are ratiometric: the ADC and the DAC both use VDD as the reference. The library converts volts to `raw_fx3` as `V × 4096 × 8 / VDD` (rounded), using the supply voltage given to the constructor.

### Common reference (Vref)

Vref is an input of the module (default VDD / 2, limited to 0 V - VDD). **Every input is taken relative to Vref**; modes with a signed output work as

$$y = V_{ref} + H(x - V_{ref})$$

Changing Vref does not reset any state; for example, a slow HPF settles to the new Vref with its own time constant.

* **Input and output around Vref:** 1st-order filter, Biquad, PGA, envelope follower, rectifier, integrator / differentiator, LOG, oscillator, modulator, noise (the input of the VCO, the modulator, the PWM modulator and the V/F converter is taken from Vref).
* **Input around Vref, output a magnitude from 0 V:** ANTILOG, squarer, square rooter, true RMS.
* **Absolute (0 V based):** comparator, clipper and dead-zone thresholds (the dead zone outputs around Vref).
* **No signal center:** bypass, slew rate limiter, sample & hold, delay.

The analog stages cannot reach the supply rails (about 0.1 V from 0 V and from VDD); keep signals within this range, or move them with `GLOBAL_OFFSET`. Outputs that are magnitudes from 0 V lose their lowest part to this headroom; lift them with `GLOBAL_OFFSET` when needed. When modules are chained, set the Vref of the next module to the level that the previous module outputs for a zero signal (its Vref plus its offset), so that the offset cancels.

### Output stage

The DSP core returns `raw_fx3`; the output stage combines the calibration gain, the inversion, the DC offsets and the rounding in one multiply-add, so the signal is rounded to an integer code only once:

$$\text{out} = \left\lfloor \frac{y \cdot G + O}{2^{18}} \right\rfloor$$

where *y* is the DSP output (`raw_fx3`, limited to ±8192 LSB), *G* is the calibration gain (`uq1_15`, negative when the output is inverted) and *O* holds the user offset and the calibration offset (`raw_fx3`, scaled to 15 more fractional bits) plus 0.5 LSB for rounding. To keep every product within 32 bits, *G* is split as ±1.0 + δ and *O* into its `raw_fx3` part and the remaining fraction; the result is the same as the formula above. The result is saturated to 0 - 4095.

### Calibration

The gain and offset calibration values are stored in flash (NVM) and loaded at start-up. They correct the combined error of the input and output signal chains (measured from AIN to AOUT in bypass mode); the ADC and the DAC are not calibrated separately. A module that has not been calibrated uses gain 1.0 and offset 0. See the `NVM_*` registers and the commands below.

---

## 4. I²C Interface

The module is an I²C target at address 0x40 - 0x43 (7-bit), selected by two solder jumpers on mimic01 and read at power-up (mimic00: 0x40). Registers are 8-bit wide and addressed with auto-increment. Registers 0x00 - 0x0F are read-only; 0x10 - 0x3F are kept in RAM and can be read back.

### Register map

| Address | Name | Access | Format | Description |
| --- | --- | --- | --- | --- |
| 0x00 | `SYSTEM_ID` | R | uint8 | WHO_AM_I, 0x4D (`'M'`). |
| 0x01 | `SYSTEM_HW_VERSION` | R | uint8 | Hardware version. |
| 0x02 - 0x05 | `SYSTEM_BUILD_DATE` | R | uint32 | Firmware build date, hex `0xYYYYMMDD` (e.g. 0x20261025). |
| 0x06 - 0x09 | `SYSTEM_UID` | R | uint32 | Unit ID: 32-bit FNV-1a hash of the MCU unique ID. |
| 0x0A | `SYSTEM_STATUS` | R/C | flags | Bit 0: ready, bit 1: signal saturation, bit 2: I²C error, bit 3: ADC overrun, bit 4: deadline miss (a sample was not finished in time, see 2. Cycle budget). Error bits are cleared on read. |
| 0x0B | `SYSTEM_ADC_SEQ` | R | uint8 | Sequence number of `SYSTEM_ADC_VAL` (+1 per update, wraps at 256). Reading it latches SEQ and VAL together; read 0x0B - 0x0D in one transaction. |
| 0x0C - 0x0D | `SYSTEM_ADC_VAL` | R | uint16 `raw_fx3` | DSP input (latched by the 0x0B read): the latest ADC sample, or the rounded average of 2^N samples when decimated. |
| 0x0E - 0x0F | `SYSTEM_CPU_CYCLES` | R/C | uint16 | Peak cycles of the primary ISR since the last read. |
| 0x10 | `GLOBAL_FLAGS` | R/W | flags | Bit 7: invert output, bit 6: output open (Hi-Z), bit 4: servo enable (1x series only, reserved). |
| 0x11 | `GLOBAL_DECIMATION_N` | R/W | uint8 | Processing rate = 200 kHz / 2^N, N = 0 - 16 (larger values are clamped). |
| 0x12 - 0x13 | `GLOBAL_OFFSET` | R/W | int16 `raw_fx3` | User output offset. |
| 0x14 - 0x15 | `GLOBAL_VREF` | R/W | int16 `raw_fx3` | Common reference voltage (default VDD / 2). |
| 0x16 | `GLOBAL_TEST_PAD` | R/W | uint8 | Signal on the test pad (experimental, see [experimental_modes.md](experimental_modes.md)). |
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
| 0x02 | `FILTER_1ST` | [0] type (0: LPF, 1: HPF, 2: all-pass), [1-2] G mant, [3] G shift (see below) |
| 0x03 | `FILTER_BIQUAD` | [0] type, [1-10] coefficients (see below) |
| 0x04 | `COMPARATOR` | [0] type (0: window, 1: Schmitt), [1-2] upper `raw_fx3`, [3-4] lower `raw_fx3` |
| 0x05 | `CLIPPER` | [0] type (0: hard, 1: soft, 2: dead zone), [1-2] upper `raw_fx3`, [3-4] lower `raw_fx3` |
| 0x06 | `RECTIFIER` | [0] type (0: full, 1: half), [1] polarity (0: positive, 1: negative) |
| 0x07 | `ENVELOPE_FOLLOWER` | [0] polarity (0: positive, 1: negative), [1-2] 1 − decay mant12, [3] 1 − decay shift (0: peak hold) |
| 0x08 | `LOG` | [0] type (0: log, 1: antilog), [1-2] slope `raw_fx3` per decade (signed), [3-4] unit `raw_fx3` (> 0) |
| 0x09 | `INTEGRATOR` | [0] type (0: differentiator, 1: integrator), [1-2] K mant `q0_15`, [3] K exp int8, [4-5] c mant12, [6] c shift (11 - 32; 0: none) (see below) |
| 0x0A | `SLEW_RATE_LIMITER` | [0-1] maximum change per sample `raw_fx3` (≥ 0) |
| 0x0B | `SAMPLE_AND_HOLD` | [0-1] period, [2-3] track time (samples, uint16) |
| 0x0C | `DELAY` | [0-1] delay (samples, uint16, up to 4095) |
| 0x0D | `OSCILLATOR` | [0] waveform (0: sine, 1: triangle, 2: square, 3: sawtooth), [1-4] phase increment uint32, [5-6] amplitude `raw_fx3`, [7-8] VCO gain mant int16, [9] VCO gain exp int8 (−16 - 4; mant 0: fixed frequency) |
| 0x0E | `MULTIPLIER` | [0] type (0: squarer, 1: square rooter), [1-2] unit `raw_fx3` (> 0) |
| 0x0F | `TRUE_RMS` | [0-1] 1 − a mant12, [2] 1 − a shift (13 - 32) |
| 0x10 | `MODULATOR` | [0] carrier waveform, [1-4] phase increment, [5-6] carrier amplitude `raw_fx3`, [7-8] gain mant int16, [9] gain exp int8 |
| 0x11 | `PWM_MODULATOR` | [0] carrier waveform, [1-4] phase increment, [5-6] full scale `raw_fx3` |
| 0x12 | `VF_CONVERTER` | [0-3] phase increment (frequency at Vref), [4-5] gain mant int16, [6] gain exp int8 |
| 0x13 | `NOISE` | [0] color (0: white, 1: pink), [1-2] amplitude `raw_fx3` |

### 0x00 Bypass
Voltage follower. The output stage (calibration, inversion, offset) still applies.

### 0x01 PGA
$y = V_{ref} + G (x - V_{ref})$ with $G = \text{mant} \cdot 2^{\text{exp}}$ (exp 0 - 15), computed as one multiply and one rounded shift.

### 0x02 1st-order filter
RC low-pass, high-pass or all-pass (phase shifter) filter around Vref, implemented as a **TPT one-pole filter**: the integrator of the analog RC filter discretized with trapezoidal integration, which gives the bilinear-transform response (exact at $f_c$). With $g = \tan(\pi f_c / f_s)$ and $G = g / (1 + g)$:

```
v = G (x - s),   lp = v + s,   s = lp + v
LPF = lp,  HPF = x - lp,  APF = lp - hp = 2 lp - x
```

The all-pass has gain 1 at every frequency and a phase of −90° at $f_c$ (0° to −180°). The state has 15 fractional bits like the Biquad states. $G \cdot 2^{12}$ is sent as a 12-bit mantissa (2048 - 4095) and a shift. Limits applied by the library: $f_s / 100000 \le f_c \le 0.24 f_s$.

### 0x03 Biquad (2nd-order filter)
LPF, HPF, BPF (0 dB peak gain), notch, all-pass (phase shifter, $x - 2k\,bp$: gain 1, −180° at $f_c$), or unity (input passed through with the same CPU load, for profiling).

Implemented as a **TPT (topology-preserving transform) state-variable filter**: the integrators of an analog state-variable filter are discretized with trapezoidal integration, which gives the same response as a bilinear-transform biquad (Audio EQ Cookbook). With $g = \tan(\pi f_c / f_s)$ and $k = 1/Q$:

```
v  = x - (k + g) s1 - s2,   hp = v / (1 + g k + g^2)
bp = g hp + s1,   s1 = bp + g hp
lp = g bp + s2,   s2 = lp + g bp
LPF = lp,  HPF = hp,  BPF = k bp,  NOTCH = x - k bp,  APF = x - 2 k bp
```

The signals are `raw_vref_fx3` and the two states have 15 fractional bits, so low cutoffs keep working without decimation. The coefficients are sent as 12-bit mantissas with shifts, so their resolution does not depend on $f_c$:

| Bytes | Content |
| --- | --- |
| [0] | type: 0 LPF, 1 HPF, 2 BPF, 3 notch, 4 unity, 5 all-pass |
| [1-2], [3] | $g \cdot 2^{12}$ = mant >> shift (mant 2048 - 4095) |
| [4-5] | $k + g$, 10 fractional bits |
| [6-7], [8] | $e = 1 - 1/(1 + g k + g^2)$ = mant >> shift |
| [9-10] | $k$, 11 fractional bits |

Limits applied by the library: $f_s / 100000 \le f_c \le 0.24 f_s$ (2 Hz - 48 kHz at 200 kHz) and $0.1 \le Q \le 10$.

### 0x04 Comparator
Digital output route (GPIO). Window: HIGH when the input is above the upper or below the lower threshold. Schmitt: HIGH above the upper threshold, LOW below the lower threshold, otherwise unchanged.

### 0x05 Clipper
Limits the input to the lower - upper range (absolute voltages).

* **Hard:** passes the input unchanged between the limits. With both limits at the same voltage, the output stays at that voltage whatever the input: a constant voltage source that does not change Vref, with the output calibration applied as for any signal (`setConstantVoltage()` in the library).
* **Soft:** a cubic curve like the smooth saturation of a diode or transistor limiter. With the center $c$ = (upper + lower) / 2, $h$ = upper − $c$ and $d = x - c$: $y = c + d - \frac{4 d^3}{27 h^2}$ for $|d| < 1.5 h$, and the limit beyond. The gain is 1 at the center and the curve meets the limit with zero slope.

* **Dead zone** (type 2): a diode dead-band circuit: the output is Vref while the input is between the limits (absolute voltages, like the clipper), and Vref plus the distance beyond the limit outside, so the transfer curve has slope 1 outside and no step:

$$y = \begin{cases} V_{ref} + (x - \text{upper}) & x > \text{upper} \\ V_{ref} & \text{lower} \le x \le \text{upper} \\ V_{ref} + (x - \text{lower}) & x < \text{lower} \end{cases}$$

Small signals and noise around the center are removed (for example the backlash of a control loop or the noise of a sensor at rest); larger signals pass, moved toward Vref by the limit. An upper limit below the lower one is taken as equal to the lower one.

### 0x06 Rectifier
Folds the signal at Vref. Full-wave positive: $V_{ref} + |x - V_{ref}|$; half-wave positive: $\max(x, V_{ref})$; negative polarity mirrors both.

### 0x07 Envelope follower
Follows the peak of the input above Vref (positive) or below Vref (negative) and decays by the decay factor $d$ per sample. $1 - d$ is sent as a 12-bit mantissa and a shift, so time constants from a few samples to minutes have the same resolution; $1 - d = 0$ holds the peak. The tracked magnitude keeps 15 fractional bits below `raw_fx3`, so long decays continue all the way down instead of stopping at the truncation. The output is on the side of the tracked peak, as a peak detector with the diode reversed: Vref + magnitude (positive), Vref − magnitude (negative). (Firmware 20261123 and earlier output Vref + magnitude for both polarities.)

### 0x08 LOG
Logarithmic amplifier: $y = V_{ref} + \text{slope} \cdot \log_{10}((x - V_{ref}) / \text{unit})$: the input is a magnitude above Vref, and the output is Vref when $x - V_{ref}$ = unit. Set Vref to 0 V for a sensor whose zero is 0 V (a photodiode, for example); keep Vref equal to that of a preceding envelope follower or rectifier (positive polarity) to get its magnitude in decades. Antilogarithmic: $y = \text{unit} \cdot 10^{(x - V_{ref}) / \text{slope}}$, a magnitude from 0 V. Implemented with per-mode tables (64 segments per octave) built when the mode is configured, and linear interpolation.

With the supply headroom (about 0.1 V from each rail), a 0 V based input or output spans about 1.7 decades at VDD = 5 V.

### 0x09 Integrator / differentiator
The op-amp integrator (input R, feedback C, with a leak resistor across the capacitor) and differentiator (input C with a series resistor, feedback R). Both use one update, with $u$ the input relative to Vref for the integrator and its change per sample for the differentiator:

$$m \leftarrow m + c\,(K u - m), \qquad y = V_{ref} + m$$

* **Integrator with leak:** $u = x - V_{ref}$, $K = \tau_{leak} / \tau_i$ (the DC gain), $c = 1 - e^{-T_s/\tau_{leak}}$: a 1st-order low-pass with gain, as the analog circuit is.
* **Integrator without leak** (c shift 0): $m \leftarrow m + K u$ with $K = T_s / \tau_i \le 1$.
* **Differentiator:** $u = x[n] - x[n-1]$, $K = \tau_d f_s$, $c = 1 - e^{-T_s/\tau_{limit}}$ for the band limit of the series resistor (c shift 0: $c = 1$, the plain difference of two samples).

$K = \text{mant} \cdot 2^{\text{exp} - 15}$; $c = \text{mant} / 2^{\text{shift}}$. The firmware computes the update as $m \leftarrow m + G u - c\,m$ with $G = c K$ (made from the two mantissas when the settings arrive, relative error below $2^{-15}$), so only $m$ is limited: $K u$ itself may be far larger, as with a large DC gain or a fast edge into the differentiator. $m$ is limited to the DAC range (0 - 4095 relative to Vref, made again when Vref changes), as the op-amp output stops at the supply rail: a saturated integrator leaves the rail as soon as the input turns. With an output offset or inversion the limit stays at this range, so it differs from the actual output range by that offset. $m$ keeps 14 fractional bits below `raw_fx3`, and the remainder of every right shift is carried to the next sample, so small inputs, small $c$ and large DC gains accumulate exactly: there is no dead band even with time constants of seconds. With a leak, $m$ enters $c\,m$ at 1/4 `raw_fx3` (rounded), the only rounding. The output is kept when the settings change.

### 0x0A Slew rate limiter
Limits the change of the output per sample (`raw_fx3`, so the smallest step is 1/8 LSB per processed sample). The output starts at mid-scale when the mode is selected.

### 0x0B Sample & hold
Tracks the input for the first *track* samples of every *period* samples and holds the last value for the rest. track = 1 gives a sample & hold; larger values give a track & hold. The held value starts at mid-scale when the mode is selected.

### 0x0C Delay
Delays the input by the given number of samples (ring buffer of 4096 samples), like a 4096-stage bucket-brigade delay line.

### 0x0D Oscillator
A function generator / VCO made by direct digital synthesis (DDS): a 32-bit phase accumulator, and a waveform computed from the phase.

$$y = V_{ref} + A\,w(\varphi), \qquad \varphi \leftarrow \varphi + \text{inc}, \qquad f = f_0 + k (x - V_{ref}), \qquad f_0 = \text{inc} \cdot f_s / 2^{32}$$

Every waveform is 0 at phase 0 and rises, so the output starts at Vref when the mode is selected:

| [0] | Waveform $w(\varphi)$ |
| --- | --- |
| 0 | Sine: a 256-segment table with linear interpolation (error below 1e-4 of the amplitude) |
| 1 | Triangle: +1 at a quarter period, −1 at three quarters |
| 2 | Square: +1 in the first half period, −1 in the second |
| 3 | Sawtooth: rises from −1 to +1 over the period (the step is at half the period) |

Other values give the sine. The square and the sawtooth change within one sample, so their harmonics above $f_s/2$ fold back (alias); like the sine, they are best kept well below 20 kHz.

* **Fixed frequency** ($k = 0$): $f_0$ (0 - $f_s/2$; resolution 0.05 mHz at 200 kHz). With $f_0 = 0$ the output stays at Vref, which makes the module a constant voltage source (set by Vref).
* **VCO** ($k \ne 0$): the frequency follows the input (linear FM, limited to 0 - $f_s/2$), with $k$ = mant × 2^exp as a phase increment per `raw_fx3`. For an exponential (1 V/oct) control, put an ANTILOG module in front.

The phase continues when the payload changes (like changing the frequency or the amplitude of an analog oscillator) and starts at 0 when the mode is selected. The output is analog. Sine waves above about 20 kHz have few samples per cycle and depend on the anti-imaging filter. With decimation the frequency is divided by 2^N, like every other time constant.

The modulator, the PWM modulator and the V/F converter (0x10 - 0x12) use the same DDS core and the same waveforms.

### 0x0E Multiplier
The squarer (an analog multiplier with both inputs tied) and the square rooter (a multiplier in the feedback of an op-amp). Both take the input relative to Vref ($d = x - V_{ref}$) and output a magnitude from 0 V, like ANTILOG.

* **Squarer:** $y = d^2 / \text{unit}$, rounded once. Squaring a sine doubles its frequency ($\sin^2 = (1 - \cos 2\omega t) / 2$); it also gives a value proportional to power.
* **Square rooter:** $y = \sqrt{d \cdot \text{unit}}$, 0 below Vref. For a sensor with a zero offset (a differential pressure sensor at VDD / 2, for example), set Vref to its zero: the output is then proportional to the flow ($v \propto \sqrt{\Delta p}$). Computed with a 193-point table and linear interpolation after normalizing the argument (error below 0.15 `raw_fx3` before rounding).
* **Squarer → square rooter:** with the same unit in both, a squarer module followed by a square-rooter module gives $|d|$ back; with different units the gain is $\sqrt{\text{unit}_{\text{rooter}} / \text{unit}_{\text{squarer}}}$. A squarer → 1st-order LPF → square-rooter chain is an RMS converter, but every link is quantized to 12 bits and the squarer loses the bottom of its range to the supply headroom, so the chain is limited to large signals (an error of a few percent at 0.8 V amplitude). This is why RMS converters are built as one circuit (0x0F).

### 0x0F True RMS
A true RMS-to-DC converter: $m \leftarrow m + (1 - a)(d^2 - m)$, $y = \sqrt{m}$ with $d = x - V_{ref}$, a magnitude from 0 V. The averaging time constant is set by $1 - a$, like the averaging capacitor of an RMS converter IC, and the gain is 1. The mean square is kept in 64 bits, so the average has no dead band even for time constants of seconds and small signals. To keep the peak cycle count low, the work is split over two samples: every square enters the mean, and the square root is computed and the output refreshed every second sample. Decimation averages the input before the DSP and removes the AC signal, and the processing with decimation does not fit in one sample period, so use the true RMS mode without decimation.

### 0x10 Modulator (balanced modulator: AM / ring)
A multiplier-based modulator (MC1496): the amplitude of the internal carrier follows the input,

$$y = V_{ref} + (A + k (x - V_{ref}))\,w(\varphi)$$

with the payload of the oscillator ($k$ = mant × 2^exp as a gain). The library sends $k = A m$ for a modulation depth $m$ per volt (AM). Like a multiplier, the carrier is inverted below −100 % modulation; the amplitude is limited to ±VDD × 2. **$A = 0$ gives the balanced (ring, DSB) modulator** $y = V_{ref} + k (x - V_{ref})\,w(\varphi)$: the sum and difference frequencies without the carrier; with the square wave it is a chopper.

### 0x11 PWM modulator
A comparator against an internal carrier (natural sampling), on the digital output route: HIGH while $x - V_{ref} > A\,w(\varphi)$. With the triangle both edges move; with the sawtooth one edge is fixed. The duty cycle goes from 0 % at $V_{ref} - A$ to 100 % at $V_{ref} + A$; each edge falls on a sample, so the resolution is $f_0 / f_s$ (0.5 % at 1 kHz).

### 0x12 V/F converter
The frequency computation of the VCO with a square output (50 % duty), on the digital output route, like an LM331 followed by a counter: $f = f_0 + k (x - V_{ref})$, limited to 0 - $f_s/2$. The output stays HIGH when the frequency is 0. A single period varies by one sample (±5 µs); the average frequency is exact. The digital route keeps the edges sharp: on the analog route the anti-imaging filter would round them and reduce the amplitude above 20 kHz (for an analog square wave, use the VCO with the square waveform).

### 0x13 Noise generator
* **White** (color 0): xorshift32, uniform within ±A (rms $A/\sqrt{3}$).
* **Pink** (color 1, −3 dB/octave): the Voss-McCartney method. Twelve random rows, row $k$ renewed every $2^{k+1}$ samples (one row per sample, chosen by the trailing zeros of a counter), plus a white part renewed every sample. The sum is scaled to an rms of $A/3$ and limited to ±A (beyond 3 σ). At 200 kHz the slope holds from about 25 Hz to $f_s/2$; decimation moves the band down by 2^N.

The noise keeps its state when the payload changes, and restarts from the same seed when the mode is selected. The input is not used.
