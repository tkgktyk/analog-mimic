# Experimental Functions

These functions are implemented in the firmware, but not yet verified on hardware and in application examples, and not yet released in the Arduino library (they are available through `AnalogMimicExperimental.h`). Their behaviour may change before they are released. When a function is released, its section moves to [firmware_reference.md](firmware_reference.md).

Every mode (0x00 - 0x13) was released with the library 0.3.0; new modes will start here.

## Test pad (register 0x16)

`GLOBAL_TEST_PAD` (0x16) selects the signal on the test pad (PA2 on mimic01); `begin()` of the library sets it to 0.

| 0x16 | Signal |
| --- | --- |
| 0 | Off: PA2 not driven (analog, Hi-Z), as after reset |
| 1 | Sample clock: a 100 kHz square wave that toggles at every sample (TIM2 update resets TIM1; TIM1 clocks TIM15, which divides it by 2) |

Other values are off. The timers run in hardware, so the sample clock adds no load to the ISR. It is the reference for the output timing: with an oscillator square wave at $f_s/2$ (analog route) or a V/F converter at $f_s/2$ (digital route), the output also changes at every sample. The output timing build (`make dac`) keeps the sample clock on.
