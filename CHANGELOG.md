# Changelog

All notable changes to this driver. This project follows a simple
`MAJOR.MINOR.PATCH` scheme; the meson version is the source of truth.

## 0.2.0 — Goodix GXFP51A7 (MilanL) support

Adds the Goodix **GXFP51A7** SPI fingerprint sensor of the Huawei MateBook 13
2019 (`WRT-WX9`, ACPI HID `GXFP51A7`, MilanL backend, chip `0x2205`, sensor
type 3, 132×112, firmware `GF3288_ST411SEC_APP_14003`), alongside the existing
GXFP5187 support. The backend, reset line and PSK address are now selected
automatically from the ACPI id.

### Fixed

- **Image capture: `0x20` answered with a `0xD0` TLS-reconnect request.**
  The post-handshake plaintext `0xD4` was issued in the same instant as the TLS
  server `Finished`. The MCU acknowledges it but never advances its TLS state
  (the state block stays `version=4`), and answers the image request with
  `d0 03 00 04 00 d3` instead of the `0xB0` image record. A settle before the
  `0xD4` advances the MCU to `version=6`, after which the image record is
  delivered and decodes to 22189 bytes (132×112, 12-bit). Default 50 ms;
  override with `GOODIXTLS_D4_DELAY_MS`.
- **MilanL FDT scan-base derivation.** MilanL (`gf_milanl.c`) derives
  `v = ((measured >> 1) << 8) | (measured >> 1)` (both bytes equal), and its up
  base adds the per-unit delta first. The ChicagoHS form
  (`((measured >> 1) << 8) | 0x80`) was being used, which sent the wrong scan
  base.

### Added

- Per-firmware PSK address: `0x20007f14` for `GF3288_ST411SEC_APP_14003`
  (GXFP51A7), selected automatically next to `0x20007f0c` for `APP_11033`.
- Per-board reset defaults: gpiochip0 line **58, active-low** (GXFP5187) and
  line **264, active-high** (GXFP51A7). The GXFP51A7 line and polarity were
  decoded from the ACPI `_CRS`/DSDT and confirmed on hardware.
- `GOODIXTLS_WRITE_GAP_US` (default `2000`) documents the split header/body
  write gap that Windows marks REQUIRED; without it the TLS handshake is flaky
  on the GXFP51A7 (~50 %).
- `GOODIXTLS_D4_DELAY_MS` (default `50`), `GOODIXTLS_D0_REINIT`,
  `GOODIXTLS_FULL_INIT`, `GOODIXTLS_TLS_OK`, `GOODIXTLS_SEQ`,
  `GOODIXTLS_SETTLE_US`, `GOODIXTLS_TIMING_SCALE` runtime knobs.

### Changed

- The `GXFP51A7` ACPI id is in the device id table; the `spidev` udev rules bind
  and expose both `GXFP5187` and `GXFP51A7`.
- Fixed the second udev rule: `KERNELS` is a glob, not a regex, so the previous
  `spi-GXFP5187:00|spi-GXFP51A7:00` matched nothing. It is now
  `spi-GXFP51*`.
- Device display name is now `Goodix GXFP5187/GXFP51A7 SPI (TLS-PSK)`.

## 0.1.0 — initial GXFP5187 support

Original driver by Benjamin Allègre (https://github.com/Sigfrodr): GXFP5187 SPI,
TLS-PSK from the sensor's own RAM, open descriptor matcher, TOD driver for
libfprint.
