# Experimental Modes

These modes are implemented in the firmware, but not yet verified on hardware and in application examples, and not yet released in the Arduino library (they are available through `AnalogMimicExperimental.h`). Their payloads and behaviour may change before they are released. When a mode is released, its section moves to [firmware_reference.md](firmware_reference.md).

The conventions (`raw_fx3`, Vref, output stage, payload offsets from `MODE_PAYLOAD` 0x21, type at byte [0]) are those of the firmware reference.

| ID | Mode | Payload |
| --- | --- | --- |
| 0x05 | `CLIPPER` (type 2) | [0] type 2: dead zone, [1-2] upper `raw_fx3`, [3-4] lower `raw_fx3` (the clipper itself, types 0 and 1, is released) |
| 0x08 | `LOG` | [0] type (0: log, 1: antilog), [1-2] slope `raw_fx3` per decade (signed), [3-4] unit `raw_fx3` (> 0) |
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

## Common reference (Vref)

* **Input and output around Vref:** LOG, oscillator, modulator, noise (the input of the VCO, the modulator, the PWM modulator and the V/F converter is taken from Vref).
* **Input around Vref, output a magnitude from 0 V:** ANTILOG, squarer, square rooter, true RMS.
* **Absolute limits, output around Vref:** dead zone (clipper type 2).
* **No signal center:** slew rate limiter, sample & hold, delay.

Outputs that are magnitudes from 0 V lose their lowest part to the supply headroom (about 0.1 V); lift them with `GLOBAL_OFFSET` when needed. For a module after one of them, set its Vref to that offset, so that the offset cancels.

## Modes

### 0x05 Clipper, type 2: dead zone
A diode dead-band circuit: the output is Vref while the input is between the limits (absolute voltages, like the clipper), and Vref plus the distance beyond the limit outside, so the transfer curve has slope 1 outside and no step:

$$y = \begin{cases} V_{ref} + (x - \text{upper}) & x > \text{upper} \\ V_{ref} & \text{lower} \le x \le \text{upper} \\ V_{ref} + (x - \text{lower}) & x < \text{lower} \end{cases}$$

Small signals and noise around the center are removed (for example the backlash of a control loop or the noise of a sensor at rest); larger signals pass, moved toward Vref by the limit. An upper limit below the lower one is taken as equal to the lower one.

### 0x08 LOG
Logarithmic amplifier: $y = V_{ref} + \text{slope} \cdot \log_{10}((x - V_{ref}) / \text{unit})$: the input is a magnitude above Vref, and the output is Vref when $x - V_{ref}$ = unit. Set Vref to 0 V for a sensor whose zero is 0 V (a photodiode, for example); keep Vref equal to that of a preceding envelope follower or rectifier to get its magnitude in decades. Antilogarithmic: $y = \text{unit} \cdot 10^{(x - V_{ref}) / \text{slope}}$, a magnitude from 0 V. Implemented with per-mode tables (64 segments per octave) built when the mode is configured, and linear interpolation.

With the supply headroom (about 0.1 V from each rail), a 0 V based input or output spans about 1.7 decades at VDD = 5 V.

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
