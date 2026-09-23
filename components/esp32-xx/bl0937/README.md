# BL0937

## Overview

ESP-IDF driver for the **BL0937** single-phase energy metering IC (pin-compatible
with the HLW8012 / CSE7759 family).

The BL0937 reports measurements as *pulse frequencies* on two pins:

- **CF** — active power, always active.
- **CF1** — voltage **or** current, whichever the **SEL** pin currently selects.

Because CF1 is shared, the driver time-multiplexes it: it holds `SEL = 1` for
half of the sample period to measure voltage, then `SEL = 0` for the other half
to measure current, and publishes one complete snapshot per full period.

Pulses are counted in hardware using the **PCNT** peripheral. On targets without
PCNT — or when PCNT units are already claimed by another driver — the component
falls back automatically to GPIO edge interrupts. Frequencies are converted to
volts, amps, watts and accumulated watt-hours using the datasheet constants and
your board's shunt value, divider ratio and reference voltage.

## Supported Platform

- **Platform:** Espressif ESP32 family
- **MCU:** ESP32, ESP32-S2, ESP32-S3, ESP32-C2, ESP32-C3, ESP32-C6, ESP32-H2
- **SDK Version:** ESP-IDF **v5.0 or later**

> The PCNT path requires a target with the pulse counter peripheral
> (`SOC_PCNT_SUPPORTED`). ESP32-C2 and ESP32-H2 have no PCNT and always use the
> GPIO ISR path — this is detected automatically, no configuration needed.

## Features

- Voltage (V), current (A), active power (W) and cumulative energy (Wh).
- Hardware pulse counting via PCNT, with an automatic GPIO ISR fallback.
- Automatic SEL time-multiplexing with a configurable settle delay.
- Configurable PCNT glitch filter for noisy mains environments.
- Per-quantity `valid_*` flags, so a zero reading is distinguishable from
  "no pulses seen yet".
- Thread-safe snapshot read — `bl0937_get()` returns a copy taken under a spinlock.
- Runtime calibration multipliers for voltage, current and power.
- Full `init` / `start` / `stop` / `deinit` lifecycle; the driver releases its
  PCNT units and ISR handlers on deinit.
- Every setting available through **Kconfig** (`idf.py menuconfig`) *and*
  overridable at runtime through `bl0937_config_t`.

## Folder Structure

```
EB_BL0937/
├── bl0937/                     # the ESP-IDF component
│   ├── CMakeLists.txt
│   ├── Kconfig                 # menuconfig options (BL0937 Energy Meter)
│   ├── idf_component.yml       # component manifest
│   ├── include/
│   │   └── bl0937.h            # public API
│   └── src/
│       └── bl0937.c            # driver implementation
├── examples/
│   └── basic_read/             # standalone example project
│       ├── CMakeLists.txt
│       ├── sdkconfig.defaults
│       └── main/
│           ├── CMakeLists.txt
│           └── main.c
├── CHANGELOG.md
├── LICENSE
└── README.md
```

## Dependency

ESP-IDF built-in components only — no external or third-party libraries.

| Dependency  | Source  | Purpose                             |
|-------------|---------|-------------------------------------|
| `driver`    | ESP-IDF | GPIO configuration, PCNT peripheral |
| `esp_timer` | ESP-IDF | Periodic half-period sampling timer |
| `freertos`  | ESP-IDF | Spinlock / critical sections        |

Minimum ESP-IDF version: **5.0**.

## Getting Started

### 1. Add the component

**Option A — Git submodule (recommended):**

```bash
git submodule add https://github.com/elecbitstech/EB_BL0937.git libs/EB_BL0937
```

Then point your project's top-level `CMakeLists.txt` at the repository root:

```cmake
set(EXTRA_COMPONENT_DIRS "libs/EB_BL0937")
```

**Option B — Copy the component folder:**

Copy the inner `bl0937/` directory into your project's `components/` directory.

### 2. Wire the hardware

| BL0937 pin | Direction | ESP pin                     |
|------------|-----------|-----------------------------|
| CF         | output    | any input-capable GPIO      |
| CF1        | output    | any input-capable GPIO      |
| SEL        | input     | any **output-capable** GPIO |

On the original ESP32, GPIO 34–39 are input-only and must **not** be used for SEL.

### 3. Configure

```bash
idf.py menuconfig
```

Navigate to **Component config → BL0937 Energy Meter**. Set your GPIOs, then the
three electrical parameters that describe your board:

| Setting              | Meaning                                                |
|----------------------|--------------------------------------------------------|
| `BL0937_SHUNT_UOHM`  | Current shunt value in micro-ohms (1 mΩ = `1000`)      |
| `BL0937_V_DIV_RATIO` | `Vpin_rms / Vline_rms` — a 1:2040 divider is `0.00049` |
| `BL0937_VREF_V`      | IC reference voltage, typically `1.218`                |

### 4. Use it

```c
#include "bl0937.h"

bl0937_config_t cfg = bl0937_config_default();   // from Kconfig
ESP_ERROR_CHECK(bl0937_init(&cfg));
ESP_ERROR_CHECK(bl0937_start());

bl0937_measurements_t m = bl0937_get();
if (m.valid_power) {
    printf("%.1f W\n", m.power_w);
}
```

### 5. Calibrate

Start with all three multipliers at `1.0`, run a known load alongside a reference
meter, then set each multiplier to `reference_reading / driver_reading`.
Calibrate **voltage first**, then current, then power — power depends on both.

Store the resulting factors either in `sdkconfig` (identical for every unit) or
in NVS and apply them at runtime:

```c
bl0937_config_t cfg = bl0937_config_default();
cfg.voltage_calibration = load_from_nvs("v_cal");
cfg.current_calibration = load_from_nvs("i_cal");
cfg.power_calibration   = load_from_nvs("p_cal");
bl0937_init(&cfg);
```

## API Reference

### `bl0937_config_t`

| Field                 | Type         | Description                                                   |
|-----------------------|--------------|---------------------------------------------------------------|
| `gpio_cf`             | `gpio_num_t` | CF pin — active power pulses.                                 |
| `gpio_cf1`            | `gpio_num_t` | CF1 pin — voltage or current pulses.                          |
| `gpio_sel`            | `gpio_num_t` | SEL pin, driven by the component. Must be output-capable.     |
| `shunt_uohm`          | `int`        | Shunt resistor in micro-ohms. Required for current and power. |
| `v_div_ratio`         | `float`      | `Vpin_rms / Vline_rms`. Required for voltage and power.       |
| `vref_v`              | `float`      | IC reference voltage, typically `1.218`.                      |
| `sample_period_ms`    | `int`        | Full period; each half measures one quantity. Minimum `100`.  |
| `sel_settle_us`       | `int`        | Delay after a SEL toggle before CF1 pulses count again.       |
| `voltage_calibration` | `float`      | Multiplier applied to computed voltage.                       |
| `current_calibration` | `float`      | Multiplier applied to computed current.                       |
| `power_calibration`   | `float`      | Multiplier applied to computed active power.                  |
| `glitch_filter_ns`    | `int`        | PCNT glitch filter width in ns. `0` disables. PCNT mode only. |
| `force_isr_fallback`  | `bool`       | Skip PCNT and use GPIO interrupts even where PCNT exists.     |

### `bl0937_measurements_t`

| Field           | Type    | Description                                            |
|-----------------|---------|--------------------------------------------------------|
| `voltage_v`     | `float` | RMS line voltage in volts.                             |
| `current_a`     | `float` | RMS line current in amperes.                           |
| `power_w`       | `float` | Active power in watts.                                 |
| `energy_wh`     | `float` | Accumulated energy in watt-hours since the last reset. |
| `cf_hz`         | `float` | Raw CF frequency (active power).                       |
| `cfu_hz`        | `float` | Raw CF1 frequency measured with `SEL = 1` (voltage).   |
| `cfi_hz`        | `float` | Raw CF1 frequency measured with `SEL = 0` (current).   |
| `valid_voltage` | `bool`  | `voltage_v` is meaningful.                             |
| `valid_current` | `bool`  | `current_a` is meaningful.                             |
| `valid_power`   | `bool`  | `power_w` is meaningful.                               |

### Functions

| Function | Returns | Description |
|----------|---------|-------------|
| `bl0937_config_default(void)` | `bl0937_config_t` | Config populated from Kconfig, or built-in fallbacks if `BL0937_ENABLE` is off. Never fails. |
| `bl0937_init(const bl0937_config_t *cfg)` | `esp_err_t` | Claims GPIOs, sets up PCNT or the ISR fallback, creates the sampling timer. Does **not** start sampling. `ESP_ERR_INVALID_ARG` if `cfg` is `NULL` or `sample_period_ms < 100`; `ESP_ERR_INVALID_STATE` if already running. |
| `bl0937_start(void)` | `esp_err_t` | Starts the periodic timer. Idempotent — returns `ESP_OK` if already running. `ESP_ERR_INVALID_STATE` if not initialised. |
| `bl0937_stop(void)` | `esp_err_t` | Stops sampling. Counters and accumulated energy are retained. |
| `bl0937_deinit(void)` | `esp_err_t` | Stops if needed, deletes the timer, releases PCNT units and ISR handlers, zeroes the snapshot. |
| `bl0937_get(void)` | `bl0937_measurements_t` | Thread-safe copy of the most recent snapshot. Safe to call from any task. |
| `bl0937_reset_energy(void)` | `void` | Resets the accumulated `energy_wh` counter to zero. |
| `bl0937_div_ratio_from_resistors(float r_top_ohm, float r_bottom_ohm)` | `float` | Inline helper: `Rbottom / (Rtop + Rbottom)`. Returns `0.0f` if either value is non-positive. |

### Conversion formulas

From the BL0937 datasheet (v1.02):

```
F_CF  = 1721506 x V(V) x V(I) / Vref^2     -> active power
F_CFU =   15397 x V(V) / Vref              -> voltage
F_CFI =   94638 x V(I) / Vref              -> current
```

The driver inverts these, then scales to line quantities using `v_div_ratio`
and the shunt resistance.

## Example

A complete, buildable project lives in [`examples/basic_read`](examples/basic_read).

```bash
cd examples/basic_read
idf.py set-target esp32c3
idf.py menuconfig
idf.py build flash monitor
```

Expected output:

```
I (1310) example: BL0937 started on CF=19 CF1=6 SEL=4
I (2320) example: V=232.4 V  I=0.412 A  P=94.8 W  E=0.026 Wh   [cf=53.1Hz cfu=1731.0Hz cfi=402.4Hz]
```

## Limitations

- **Single instance.** The driver keeps one static context, so only one BL0937
  can be driven per firmware image.
- **Uncalibrated by default.** All three multipliers default to `1.0`. Absolute
  accuracy depends entirely on your shunt tolerance, divider ratio, and the
  calibration step above.
- **CF is sampled per half-period.** The published `cf_hz` is the count from the
  most recently completed half-window, not an average across the full period. At
  low power, where CF pulses are sparse, expect visible sample-to-sample
  variation — average in your application if you need a stable reading.
- **`energy_wh` is volatile.** It accumulates in RAM and resets on reboot.
  Persist it yourself (NVS or a filesystem) if it must survive a power cycle.
- **No power factor, apparent power or line frequency.** The BL0937 does not
  report them, and the driver does not estimate them.
- **`sample_period_ms` is halved internally.** An odd value is truncated by the
  integer division; prefer an even number.
- **No PCNT on ESP32-C2 / ESP32-H2.** These targets always take the GPIO ISR
  path, which costs more CPU at high pulse rates.
- **Mains voltage.** The BL0937 sits on the live side of the circuit. Isolate
  properly and do not probe a live board.

## Changelog

See [CHANGELOG.md](CHANGELOG.md).

## Maintainer

**Ayesha Sheik** — <sheik.ayesha@elecbits.in>  
Elecbits Technologies Pvt. Ltd.

Issues and pull requests: <https://github.com/elecbitstech/EB_BL0937/issues>

### Attribution

Derived from [AchimPieters/esp32-bl0937](https://github.com/AchimPieters/esp32-bl0937)
(MIT). See [LICENSE](LICENSE).
