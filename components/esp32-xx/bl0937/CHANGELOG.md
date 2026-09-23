# Changelog

All notable changes to this component are documented here.
The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [1.0.0] — 2026-08-31

First public release, extracted from the Elecbits Wiser Smart Adapter firmware
and made standalone.

### Added

- `Kconfig` at component level. The `CONFIG_BL0937_*` options previously lived in
  the host application's `Kconfig.projbuild`, which meant the component could not
  be built outside that one project.
- `examples/basic_read` — a complete, buildable example project.
- `idf_component.yml` manifest naming Elecbits as maintainer, with the target
  list corrected (ESP32-C5 and ESP32-C61 removed, ESP32-H2 added).
- This changelog, a README, and an MIT `LICENSE` carrying both the upstream and
  the Elecbits copyright.

### Changed

- Calibration multipliers now default to `1.0` instead of the Wiser board's
  measured values (`3.058413` / `1.010` / `3.0155`). Those figures are specific
  to one PCB and would silently produce wrong readings on any other hardware.

### Notes

- The driver logic in `src/bl0937.c` is unchanged from the version running in
  production on the Wiser Smart Adapter.
- Derived from [AchimPieters/esp32-bl0937](https://github.com/AchimPieters/esp32-bl0937) (MIT).
