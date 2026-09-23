# stpm34

## Overview

ESP-IDF driver for the STMicroelectronics STPM34 energy-metering IC,
talked to over SPI. Reads RMS voltage/current, active/reactive/apparent
power, power factor, line frequency and phase angle, with per-channel
calibration. Fully generic and hardware-agnostic - no dependency on any
application-specific header and no dependency on any other component;
every pin, SPI host, clock speed and calibration value is supplied by the
caller. The EN/RST reset line is driven as a plain GPIO by default, or via
an optional caller-supplied callback if your board drives it through
something other than a GPIO (see Dependency and API Reference below).

## Supported Platform

- Platform: ESP32 (Espressif)
- MCU: ESP32-C3 (validated in this project); portable to any ESP32 variant with SPI master support
- SDK Version: ESP-IDF v5.5.x

## Features

- SPI communication with the STPM34, including CRC-8 frame validation
- Per-channel (1 or 2) voltage/current calibration
- RMS voltage, RMS current, active/reactive/apparent power, power factor
- Line frequency (both channels) and phase angle
- Configurable gain, auto-latch and CRC-enable via chip registers
- EN/RST driven as a plain GPIO by default, or via an optional
  caller-supplied callback (`stpm34_set_reset_callback()`) for boards that
  drive it through an I/O expander or similar - no component dependency
  either way

## Folder Structure

```
stpm34/
├── CMakeLists.txt
├── README.md
├── example/
│   └── stpm34_example.c   - minimal init + read-loop reference
├── include/
│   ├── stpm34.h           - public API
│   └── STPM3X_define.h    - register/bitfield definitions
└── stpm34.c
```

## Dependency

ESP-IDF `driver` (SPI master). Nothing else - no other component is
required, regardless of how your board drives EN/RST.

## Getting Started

1. Copy this folder into your project's `components/` directory.
2. `#include "stpm34.h"`.
3. Declare a `stpm34_t` instance and call, in order:
   `stpm34_init_handle()` -> `stpm34_begin()` -> `stpm34_set_calibration()` -> `stpm34_chip_init()`.
4. Call `stpm34_read_*()` from your own metering task, as often as you need.

The four steps above are enough to get *some* readings out of the chip.
The sections below cover the parts that actually take a bit of thought:
picking channel 1 vs 2, making the readings come out in real volts/amps
instead of whatever this board's fixed scale happens to produce, and
wiring EN/RST correctly for your hardware.

> **Note:** if you call `stpm34_*()` functions directly from your project's
> `main` component, you do **not** need to add `stpm34` to `REQUIRES` in
> `main/CMakeLists.txt` - ESP-IDF's `main` component implicitly depends on
> every other component in the build. You only need an explicit
> `REQUIRES stpm34` if you're consuming this driver from inside a
> different custom component of your own.

### Channel selection

The STPM34 measures two independent lines - "primary" and "secondary" in
the datasheet. Every `stpm34_read_*` function takes a `channel` argument:

- `channel = 1` -> primary inputs (V1/C1 on the chip)
- `channel = 2` -> secondary inputs (V2/C2 on the chip)

Which physical wire ends up being "channel 1" vs "channel 2" is purely
which pins you soldered it to - the driver has no concept of "phase A" or
"line 2", it just latches and reads whichever channel's registers you ask
for. Calibration for both channels is initialized to a safe `1.0`/`1.0`
multiplier by `stpm34_init_handle()`, so if your board only wires up
channel 1, you can simply never call anything with `channel = 2` - there's
no equivalent of "you must fill in channel 2 or it'll misbehave" here.

### Hardware scale - why your readings might be off by a fixed ratio

Unlike a driver that takes a resistor-divider/shunt config struct and
computes the raw-to-physical conversion at runtime, this driver's
volts/amps/watts conversion is three **fixed constants** in `stpm34.c`:

```c
// stpm34.c
static inline float calcRmsVolt(uint16_t value)    { return (float)value * 0.035484044f; }
static inline float calcRmsCurrent(uint32_t value) { return (float)value * 0.0002143f; }
static inline float calcPower(int32_t value)       { return (float)value * 0.0001217f; }
```

Those specific numbers are correct for *this project's* hardware: a
particular voltage-divider ratio (R1/R2) on the STPM34's voltage inputs, a
particular current-sensor sensitivity (a shunt or CT + burden resistor,
"kS" in the datasheet), and the current gain `stpm34_chip_init()` always
programs both channels to - `STPM_GAIN_16X` (see Limitations). If you
reuse this component as-is on a board with a different divider or a
different current sensor, your readings will be off by a fixed ratio, not
because anything is broken, but because these three constants describe
someone else's resistors, not yours.

The datasheet (STPM34 section 8.4.6-8.4.7) gives the relationship in
terms of your actual components:

```
VMAX_rms [V] = 0.2121 * (1 + R1/R2)
IMAX_rms [A] = 0.4243 / (AI * kS)
```

where `R1`/`R2` are your voltage-divider resistors (line side / STPM34
side), `kS` is your current sensor's volts-out-per-amp-in, and `AI` is the
numeric current gain (2/4/8/16 - this driver always uses 16). `VMAX_rms`/
`IMAX_rms` are the RMS voltage/current that would just clip the STPM34's
fixed +/-300 mV peak input, so working backward from your maximum
expected line voltage/current tells you whether your divider/sensor
choice leaves enough headroom.

You don't need to derive your own constants from scratch to use a
different board, though - since the whole conversion is linear
(`physical_value = raw * fixed_constant * calV_or_calI`), a single
`calV`/`calI` multiplier via `stpm34_set_calibration()` can absorb the
*entire* difference between this board's fixed constants and your board's
correct scale, not just a small trim. Two ways to get there:

- **Compute it directly**, if you know your actual R1/R2 and kS: work out
  what *this driver's* fixed constant assumes (solve the datasheet
  formulas above backward - the worked example below does exactly that),
  then set `calV`/`calI` to the ratio between your board's correct
  constant and this one.
- **Or just measure it** against a trusted reference instrument - see
  "Fine calibration trim" below. This works regardless of whether you
  know your exact component values, and is what you'd do anyway to soak
  up individual component tolerance (+/-1-5%, per the datasheet) even on
  hardware identical to this board.

If you're changing hardware enough that a `calV`/`calI` multiplier isn't
a good enough fit (e.g. a current sensor with a non-linear response),
the fixed constants in `calcRmsVolt`/`calcRmsCurrent`/`calcPower` are the
ones to edit directly in `stpm34.c`.

**Worked example - what this board's constants actually represent:**
solving the formulas above backward against the fixed constants shipped
in `stpm34.c` (`0.035484044` V, `0.0002143` A, `0.0001217` W per raw
count) gives roughly `1 + R1/R2 ~= 1723`, `kS ~= 3.0 mV/A`, at the fixed
`STPM_GAIN_16X`. That's consistent with a large high-side divider
resistor (hundreds of kOhm) against a small low-side resistor, and a
milliohm-range current shunt - check this project's actual schematic/BOM
for the real R1, R2 and shunt values rather than relying on this
back-calculated ratio alone.

### Fine calibration trim

Even on hardware identical to this board, expect the STPM34's raw reading
to be a little off from a trusted reference (a multimeter for voltage, a
clamp meter for current) - a few tenths of an amp, or a couple of volts,
is normal and exactly what this step is for; it's component tolerance and
per-chip Vref drift, not a bug.

```
new_cal = old_cal * (actual_reading / stpm_reading)
```

`actual_reading` is what your reference instrument reads; `stpm_reading`
is what `stpm34_read_rms_voltage()`/`stpm34_read_rms_current()` reported
at the same moment. Apply this separately for V and I, and separately per
channel:

1. Apply a steady, non-trivial load (don't calibrate near zero load - the
   ratio is noisiest there).
2. Average 10-20 samples of `stpm34_read_rms_voltage()`/
   `stpm34_read_rms_current()` a second or two apart, to smooth out
   jitter.
3. At the same load, note the actual voltage/current from your reference
   meter.
4. Compute `new_cal = old_cal * (actual / stpm_avg)` for V and I
   separately. `old_cal` starts at `1.0` (the default from
   `stpm34_init_handle()`), so on the first pass this is just
   `actual / stpm_avg`.
5. Call `stpm34_set_calibration()` with the new values and re-measure. If
   still off, repeat step 4 using the new reading as `stpm_avg` - it
   should converge within one or two passes.

**Worked example:** channel 1 still at the default (`calV = calI = 1.0`),
and at a 2-3 bulb load you measure (STPM34, averaged) `228.4 V` / `1.94 A`,
while a multimeter/clamp meter reads `231.0 V` / `2.06 A` at the same
moment:

```
new_calV = 1.0 * (231.0 / 228.4) = 1.011
new_calI = 1.0 * (2.06 / 1.94)   = 1.062
```

```c
stpm34_set_calibration(&g_meter, 1, 1.011f, 1.062f);
```

### EN/RESET wiring

**Native ESP32 GPIO:**

```c
stpm34_init_handle(&dev, /*enrst=*/ GPIO_NUM_4, cs, syn, 50);
// nothing else needed - stpm34_begin()/stpm34_chip_init() drive GPIO_NUM_4 directly
```

**Through an I/O expander (e.g. this project's FXL6408):**

```c
#include "io_expander.h"

#define STPM34_EN_PIN 1  // whichever expander pin drives STPM34 EN/RESET

static void stpm34_reset_via_expander(bool level, void *ctx)
{
    (void)ctx;
    io_write(STPM34_EN_PIN, level);
}

stpm34_init_handle(&dev, /*enrst=*/ GPIO_NUM_NC, cs, syn, 50); // enrst unused - callback drives it instead
stpm34_set_reset_callback(&dev, stpm34_reset_via_expander, NULL); // before stpm34_begin()
```

See `example/stpm34_example.c` for both patterns side by side.

## API Reference

| Function | Description |
|---|---|
| `void stpm34_init_handle(stpm34_t *dev, gpio_num_t enrst, gpio_num_t cs, gpio_num_t syn, int net_freq_hz)` | Bind logical pins and mains frequency to a `stpm34_t` instance. Pass `GPIO_NUM_NC` for `enrst` if you'll use `stpm34_set_reset_callback()` instead. |
| `void stpm34_set_reset_callback(stpm34_t *dev, stpm34_reset_fn_t fn, void *ctx)` | Optional. Register a custom EN/RST driver function, for boards where that line isn't a plain GPIO. Call after `stpm34_init_handle()`, before `stpm34_begin()`. Pass `fn = NULL` to go back to driving `pin_enrst` directly. |
| `esp_err_t stpm34_begin(stpm34_t *dev, spi_host_device_t host, gpio_num_t sck, gpio_num_t miso, gpio_num_t mosi, int clock_hz)` | Bring up the SPI bus/device for this instance. |
| `esp_err_t stpm34_reinit_spi(stpm34_t *dev)` | Remove and re-add the SPI device using the stored clock speed. |
| `bool stpm34_chip_init(stpm34_t *dev)` | Initialize STPM34 chip registers (gain, CRC, auto-latch). |
| `void stpm34_set_calibration(stpm34_t *dev, uint8_t channel, float calV, float calI)` | Set per-channel voltage/current calibration multipliers. |
| `float stpm34_read_rms_voltage(stpm34_t *dev, uint8_t channel)` | RMS voltage in volts. |
| `float stpm34_read_rms_current(stpm34_t *dev, uint8_t channel)` | RMS current in amps. |
| `float stpm34_read_active_power(stpm34_t *dev, uint8_t channel)` | Active power in watts. |
| `float stpm34_read_reactive_power(stpm34_t *dev, uint8_t channel)` | Reactive power in VAR. |
| `float stpm34_read_apparent_rms_power(stpm34_t *dev, uint8_t channel)` | Apparent power in VA. |
| `float stpm34_read_power_factor(stpm34_t *dev, uint8_t channel)` | Power factor. |
| `void stpm34_read_line_frequency(stpm34_t *dev, float *freq_hz1, float *freq_hz2)` | Line frequency for both channels. |
| `float stpm34_read_phase_angle(stpm34_t *dev, uint8_t channel)` | Phase angle. |

`channel` is 1 or 2 (the STPM34's two metering channels); calibration is
stored per-channel in `stpm34_t` and applied automatically by every
`stpm34_read_*` call.

## Example

See [`example/stpm34_example.c`](example/stpm34_example.c) for a complete
init-and-read-loop reference. It's not built by this component's
`CMakeLists.txt` - copy it into your own application code and adjust the
pins/SPI host/calibration values for your board.

## Limitations

- The voltage/current/power raw-to-physical conversion (`calcRmsVolt`/
  `calcRmsCurrent`/`calcPower` in `stpm34.c`) uses three constants fixed
  at build time for this board's actual voltage-divider ratio and current
  sensor sensitivity - see "Hardware scale" above for how to adapt them
  (or work around them via `calV`/`calI`) for a different board.
- `stpm34_chip_init()` unconditionally programs both channels to
  `STPM_GAIN_16X`. The `gain1`/`gain2` fields in `stpm34_t` exist and
  default to `STPM_GAIN_2X`, but there's no public setter to make a
  different gain take effect before `stpm34_chip_init()` runs - changing
  the configured gain currently requires editing the
  `stpm34_set_current_gain()` calls inside `stpm34_init_regs()` (and
  re-deriving the scale constants above to match).
- If neither a plain GPIO (`pin_enrst`) nor a reset callback is configured,
  EN/RST is never driven at all - only use that combination if your
  hardware ties EN/RST permanently active in circuit.
- The register read-modify-write helpers (gain, CRC-enable, auto-latch) use
  file-scope global state rather than per-instance state, so they are not
  safe to call concurrently from two tasks against two different `stpm34_t`
  instances. Fine for this project's single-meter use; a multi-meter setup
  would need that state moved into `stpm34_t`.

## Changelog

- v1.2 (2026-08-20) - Expanded Getting Started with channel selection, the hardware-scale/calibration relationship, a fine-trim calibration procedure, and both EN/RESET wiring patterns; documented the fixed-gain and fixed-scale-constant limitations.
- v1.1 (2026-08-20) - Removed the hard `io_expander` component dependency: EN/RST is now driven as a plain GPIO by default, with an optional `stpm34_set_reset_callback()` for boards that drive it through something else. `stpm34` no longer requires any other component.
- v1.0 (2026-08-20) - Extracted as a standalone, parameterized component with README and reference example.

## Maintainer

Elecbits (ODM) - vishnu.vardhan@elecbits.in
=======
# STPM34
Generic ESP-IDF driver for the STMicroelectronics STPM34 energy-metering IC over SPI — RMS voltage/current, power, power factor, frequency, and phase angle, with pluggable EN/RST control. No hardware dependencies.
